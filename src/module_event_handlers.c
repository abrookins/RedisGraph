/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "module_event_handlers.h"
#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include "graph/graphcontext.h"
#include "serializers/graphcontext_type.h"
#include "serializers/graphmeta_type.h"
#include "config.h"

// Global array tracking all extant GraphContexts.
extern GraphContext **graphs_in_keyspace;
// Flag indicating whether the running process is a child.
extern bool process_is_child;
// GraphContext type as it is registered at Redis.
extern RedisModuleType *GraphContextRedisModuleType;
// Graph meta keys type as it is registered at Redis.
extern RedisModuleType *GraphMetaRedisModuleType;

/* Both of the following fields are required to verify that the module is replicated
 * in a successful manner. In a sharded environment, there could be a race condition between the decoding of
 * the last key, and the last aux_fields, so both counters should be zeroed in order to verify
 * that the module replicated properly.*/

/* Holds the number of aux fields encountered during decoding of RDB file.
 * This field is used to represent when the module is replicating its graphs. */
uint aux_field_counter = 0 ;

/* Holds the number of graphs encountered during decoding of RDB file.
 * This field is used to represent when the module is replicating its graphs. */
uint currently_decoding_graphs = 0;

/* This callback invokes once rename for a graph is done. Since the key value is a graph context
 * which saves the name of the graph for later key accesses, this data must be consistent with the key name,
 * otherwise, the graph context will remain with the previous graph name, and a key access to this name might
 * yield an empty key or wrong value. This method changes the graph name value at the graph context to be
 * consistent with the key name. */
static int _RenameGraphHandler(RedisModuleCtx *ctx, int type, const char *event,
							   RedisModuleString *key_name) {
	if(type != REDISMODULE_NOTIFY_GENERIC) return REDISMODULE_OK;
	if(strcasecmp(event, "RENAME_TO") == 0) {
		RedisModuleKey *key = RedisModule_OpenKey(ctx, key_name, REDISMODULE_WRITE);
		if(RedisModule_ModuleTypeGetType(key) == GraphContextRedisModuleType) {
			GraphContext *gc = RedisModule_ModuleTypeGetValue(key);
			size_t len;
			const char *new_name = RedisModule_StringPtrLen(key_name, &len);
			GraphContext_Rename(gc, new_name);
		}
		RedisModule_CloseKey(key);
	}
	return REDISMODULE_OK;
}

//------------------------------------------------------------------------------
// Meta keys API
//------------------------------------------------------------------------------

// Checks if the graph name contains a hash tag between curly braces.
static bool _GraphContext_NameContainsTag(const GraphContext *gc) {
	const char *left_curly_brace = strstr(gc->graph_name, "{");
	if(left_curly_brace) {
		const char *right_curly_brace = strstr(left_curly_brace, "}");
		if(right_curly_brace) {
			return true;
		}
	}
	return false;
}

// Calculate how many virtual keys are needed to represent the graph.
static uint64_t _GraphContext_RequiredMetaKeys(const GraphContext *gc) {
	uint64_t required_keys = 0;
	RG_Config config = Config_GetModuleConfig();
	required_keys += ceil((double)Graph_NodeCount(gc->g) / config.entities_threshold);
	required_keys += ceil((double)Graph_EdgeCount(gc->g) / config.entities_threshold);
	required_keys += ceil((double)Graph_DeletedNodeCount(gc->g) / config.entities_threshold);
	required_keys += ceil((double)Graph_DeletedEdgeCount(gc->g) / config.entities_threshold);
	return required_keys;
}

static void _CreateGraphMetaKeys(RedisModuleCtx *ctx, GraphContext *gc) {
	uint meta_key_count = _GraphContext_RequiredMetaKeys(gc);
	bool graph_name_contains_tag = _GraphContext_NameContainsTag(gc);
	for(uint i = 1; i <= meta_key_count; i++) {
		RedisModuleString *meta_rm_string;
		/* Meta keys need to be in the exact shard/slot as the graph context key, to avoid graph sharding - we want to save all  the graph keys on the same shard.
		 * For that, we need to that them In so their tag hash value will be the same as the graph context key hash value.
		 * If the graph name already contains a tag, we can duplicate the graph name completely for each meta key. If not, the meta keys tag will be the graph name, so
		 * when hashing the graphcontext key name (graph name) and the graph meta key tag (graph name) the hash values will be the same. */
		if(graph_name_contains_tag) {
			// Graph already has a tag, create a meta key of "graph_name_i"
			meta_rm_string = RedisModule_CreateStringPrintf(ctx, "%s_%u", gc->graph_name, i);
		} else {
			// Graph is untagged, one must be introduced to ensure that keys are propagated to the same node.
			// Create a meta key of "{graph_name}graph_name_i"
			meta_rm_string = RedisModule_CreateStringPrintf(ctx, "{%s}%s_%u", gc->graph_name,
															gc->graph_name, i);
		}

		RedisModuleKey *key = RedisModule_OpenKey(ctx, meta_rm_string, REDISMODULE_WRITE);
		// Set value in key.
		RedisModule_ModuleTypeSetValue(key, GraphMetaRedisModuleType, gc);
		RedisModule_CloseKey(key);
		RedisModule_FreeString(ctx, meta_rm_string);
	}
	GraphEncodeContext_SetMetaKeysCount(gc->encoding_context, meta_key_count);
}

// Delete meta keys, upon RDB encode or decode finished event triggering. The decode flag represent the event.
static void _DeleteGraphMetaKeys(RedisModuleCtx *ctx, GraphContext *gc, bool decode) {
	uint key_count;
	// Get the number of meta keys required, according to the "decode" flag.
	if(decode) key_count = GraphDecodeContext_GetKeyCount(gc->decoding_context) - 1;
	else key_count = GraphEncodeContext_GetKeyCount(gc->encoding_context) - 1;
	bool graph_name_contains_tag = _GraphContext_NameContainsTag(gc);
	for(uint i = 1; i <= key_count; i++) {
		RedisModuleString *meta_rm_string;
		if(graph_name_contains_tag) {
			meta_rm_string = RedisModule_CreateStringPrintf(ctx, "%s_%u", gc->graph_name, i);
		} else {
			meta_rm_string = RedisModule_CreateStringPrintf(ctx, "{%s}%s_%u", gc->graph_name,
															gc->graph_name, i);
		}
		RedisModuleKey *key = RedisModule_OpenKey(ctx, meta_rm_string, REDISMODULE_WRITE);
		RedisModule_DeleteKey(key);
		RedisModule_CloseKey(key);
		RedisModule_FreeString(ctx, meta_rm_string);
	}
}

// Create the meta keys for each graph in the key space - used on RDB start event.
static void _CreateKeySpaceMetaKeys(RedisModuleCtx *ctx) {
	uint graphs_in_keyspace_count = array_len(graphs_in_keyspace);
	for(uint i = 0; i < graphs_in_keyspace_count; i ++) {
		_CreateGraphMetaKeys(ctx, graphs_in_keyspace[i]);
	}
}

static void _ResetDecodeStates() {
	uint graphs_in_keyspace_count = array_len(graphs_in_keyspace);
	for(uint i = 0; i < graphs_in_keyspace_count; i ++) {
		GraphDecodeContext_Reset(graphs_in_keyspace[i]->decoding_context);
	}
}

/* Delete the meta keys for each graph in the key space - used on RDB finish (save/load/fail) event.
 * The decode flag represent if the graph is after encodeing or decodeing. */
static void _ClearKeySpaceMetaKeys(RedisModuleCtx *ctx, bool decode) {
	uint graphs_in_keyspace_count = array_len(graphs_in_keyspace);
	for(uint i = 0; i < graphs_in_keyspace_count; i ++) {
		_DeleteGraphMetaKeys(ctx, graphs_in_keyspace[i], decode);
	}
}

static void _FlushDBHandler(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent,
							void *data) {
	if(eid.id == REDISMODULE_EVENT_FLUSHDB && subevent == REDISMODULE_SUBEVENT_FLUSHDB_START) {
		// If a flushall occurs during replication, stop all decoding.
		aux_field_counter = 0;
		currently_decoding_graphs = 0;
		_ResetDecodeStates();
	}
}

// Checks if the event is persistence start event.
static bool _IsEventPersistenceStart(RedisModuleEvent eid, uint64_t subevent) {
	return eid.id == REDISMODULE_EVENT_PERSISTENCE  &&
		   (subevent == REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START ||    // Normal RDB.
			subevent == REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START ||    // Preamble AOF.
			subevent == REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START  // SAVE and DEBUG RELOAD.
		   );
}

// Checks if the event is persistence end event.
static bool _IsEventPersistenceEnd(RedisModuleEvent eid, uint64_t subevent) {
	return eid.id == REDISMODULE_EVENT_PERSISTENCE &&
		   (subevent == REDISMODULE_SUBEVENT_PERSISTENCE_ENDED ||  // Save ended.
			subevent == REDISMODULE_SUBEVENT_PERSISTENCE_FAILED    // Save failed.
		   );
}

// Server persistence event handler.
static void _PersistenceEventHandler(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent,
									 void *data) {
	if(_IsEventPersistenceStart(eid, subevent)) _CreateKeySpaceMetaKeys(ctx);
	else if(_IsEventPersistenceEnd(eid, subevent)) _ClearKeySpaceMetaKeys(ctx, false);
}

static void _RegisterServerEvents(RedisModuleCtx *ctx) {
	RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_GENERIC, _RenameGraphHandler);
	RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_FlushDB, _FlushDBHandler);
	RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Persistence, _PersistenceEventHandler);
}

static void RG_ForkPrepare() {
	/* At this point, a fork call has been issued. (We assume that this is because BGSave was called.)
	 * Acquire the read-write lock of each graph to ensure that no graph is being modified, or else
	 * the child process will deadlock when attempting to acquire that lock.
	 * 1. If a writer thread is active, we'll wait until the writer finishes and releases the lock.
	 * 2. Otherwise, no write in progress. Acquire the lock and release it immediately after forking. */

	uint graph_count = array_len(graphs_in_keyspace);
	for(uint i = 0; i < graph_count; i++) {
		// Acquire each read-write lock as a reader to guarantee that no graph is being modified.
		Graph_AcquireReadLock(graphs_in_keyspace[i]->g);
	}
}

static void RG_AfterForkParent() {
	/* The process has forked, and the parent process is continuing.
	 * Release all locks. */

	uint graph_count = array_len(graphs_in_keyspace);
	for(uint i = 0; i < graph_count; i++) {
		// Release each read-write lock.
		Graph_ReleaseLock(graphs_in_keyspace[i]->g);
	}
}

static void RG_AfterForkChild() {
	/* Restrict GraphBLAS to use a single thread this is done for 2 reasons:
	 * 1. save resources.
	 * 2. avoid a bug in GNU OpenMP which hangs when performing parallel loop in forked process. */
	GxB_set(GxB_NTHREADS, 1);

	/* Mark that the child is a forked process so that it doesn't attempt invalid
	 * accesses of POSIX primitives it doesn't own. */
	process_is_child = true;
}

static void _RegisterForkHooks() {
	/* Register handlers to control the behavior of fork calls.
	 * The child process does not require a handler. */
	assert(pthread_atfork(RG_ForkPrepare, RG_AfterForkParent, RG_AfterForkChild) == 0);
}

static void _ModuleEventHandler_TryClearKeyspace(void) {
	if(aux_field_counter == 0 && currently_decoding_graphs == 0) {
		RedisModuleCtx *ctx = RedisModule_GetThreadSafeContext(NULL);
		_ClearKeySpaceMetaKeys(ctx, true);
		RedisModule_FreeThreadSafeContext(ctx);
	}
}

/* Increase the number of aux fields encountered during rdb loading. There could be more than one on multiple shards scenario
 * so each shard is saving the aux field in its own RDB file. */
void ModuleEventHandler_AUXBeforeKeyspaceEvent(void) {
	aux_field_counter++;
}

/* Decrease the number of aux fields encountered during rdb loading. There could be more than one on multiple shards scenario
 * so each shard is saving the aux field in its own RDB file. Once the number is zero, the module finished replicating and the meta keys can be deleted. */
void ModuleEventHandler_AUXAfterKeyspaceEvent(void) {
	aux_field_counter--;
	_ModuleEventHandler_TryClearKeyspace();
}

void ModuleEventHandler_IncreaseDecodingGraphsCount(void) {
	currently_decoding_graphs++;
}

void ModuleEventHandler_DecreaseDecodingGraphsCount(void) {
	currently_decoding_graphs--;
	_ModuleEventHandler_TryClearKeyspace();
}

void RegisterEventHandlers(RedisModuleCtx *ctx) {
	_RegisterForkHooks();       // Set up hooks for forking logic to prevent bgsave deadlocks.
	_RegisterServerEvents(ctx); // Set up hooks for rename and server events on Redis 6 and up.
}
