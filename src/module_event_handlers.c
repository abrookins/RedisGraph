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
#include "graph/graphmetacontext.h"
#include "serializers/graphcontext_type.h"
#include "serializers/graphmeta_type.h"
#include "util/uuid.h"

extern GraphContext **graphs_in_keyspace;  // Global array tracking all extant GraphContexts.
extern bool
process_is_child;              // Flag indicating whether the running process is a child.
extern uint64_t entities_threshold;        // The limit of number of entities encoded at once.
extern uint redis_major_version;           // The redis server major version.

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

// Checks if the graph name contains a tag between curly braces.
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
	required_keys += ceil((double)Graph_NodeCount(gc->g) / entities_threshold);
	required_keys += ceil((double)Graph_EdgeCount(gc->g) / entities_threshold);
	required_keys += ceil((double)Graph_DeletedNodeCount(gc->g) / entities_threshold);
	required_keys += ceil((double)Graph_DeletedEdgeCount(gc->g) / entities_threshold);
	return required_keys;
}

static void _CreateGraphMetaKeys(RedisModuleCtx *ctx, GraphContext *gc) {
	uint meta_key_count = _GraphContext_RequiredMetaKeys(gc);
	for(uint i = 0; i < meta_key_count; i++) {
		bool graph_name_contains_tag = _GraphContext_NameContainsTag(gc);
		char *uuid = UUID_New();
		size_t meta_key_name_length = strlen(uuid) + strlen(gc->graph_name) + 2; // graphname_uuid\0
		if(!graph_name_contains_tag) {
			meta_key_name_length += strlen(gc->graph_name) + 2; // {graphname}graphname_uuid\0
		}
		char meta_key_name[meta_key_name_length];
		if(graph_name_contains_tag) {
			sprintf(meta_key_name, "%s_%s", gc->graph_name, uuid);
		} else {
			sprintf(meta_key_name, "{%s}%s_%s", gc->graph_name, gc->graph_name, uuid);
		}
		rm_free(uuid);
		GraphMetaContext *meta = GraphMetaContext_New(gc, meta_key_name);
		RedisModuleString *meta_rm_string = RedisModule_CreateString(ctx, meta_key_name,
																	 strlen(meta_key_name));

		RedisModuleKey *key = RedisModule_OpenKey(ctx, meta_rm_string, REDISMODULE_WRITE);
		// Set value in key.
		RedisModule_ModuleTypeSetValue(key, GraphMetaRedisModuleType, meta);
		RedisModule_CloseKey(key);
		GraphEncodeContext_AddKey(gc->encoding_context, meta_key_name);
		RedisModule_FreeString(ctx, meta_rm_string);
	}
}

static void _DeleteGraphMetaKeys(RedisModuleCtx *ctx, GraphContext *gc) {
	unsigned char **keys = GraphEncodeContext_GetKeys(gc->encoding_context);
	uint key_count = array_len(keys);
	for(uint i = 0; i < key_count; i++) {
		char *meta_key_name = (char *)keys[i];
		RedisModuleString *meta_rm_string = RedisModule_CreateString(ctx, meta_key_name,
																	 strlen(meta_key_name));

		RedisModuleKey *key = RedisModule_OpenKey(ctx, meta_rm_string, REDISMODULE_WRITE);
		RedisModule_DeleteKey(key);
		RedisModule_CloseKey(key);

		RedisModule_FreeString(ctx, meta_rm_string);
		// Free the name, as it will no longer be used.
		rm_free(meta_key_name);
	}
	array_free(keys);
}

// Create the meta keys for each graph in the key space - used on RDB start event.
static void _CreateKeySpaceMetaKeys(RedisModuleCtx *ctx) {
	uint graphs_in_keyspace_count = array_len(graphs_in_keyspace);
	for(uint i = 0; i < graphs_in_keyspace_count; i ++) {
		_CreateGraphMetaKeys(ctx, graphs_in_keyspace[i]);
	}
}

// Delete the meta keys for each graph in the key space - used on RDB finish (save/load/fail) event.
static void _ClearKeySpaceMetaKeys(RedisModuleCtx *ctx) {
	uint graphs_in_keyspace_count = array_len(graphs_in_keyspace);
	for(uint i = 0; i < graphs_in_keyspace_count; i ++) {
		_DeleteGraphMetaKeys(ctx, graphs_in_keyspace[i]);
	}
}

// Delete all meta keys before the actual flush, as the flush may be out of order
static void _FlushDBHandler(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent,
							void *data) {
	if(eid.id == REDISMODULE_EVENT_FLUSHDB && subevent == REDISMODULE_SUBEVENT_FLUSHDB_START) {
		_ClearKeySpaceMetaKeys(ctx);
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

// Checks if the event is loading end event.
static bool _IsEventLoadingEnd(RedisModuleEvent eid, uint64_t subevent) {
	return eid.id == REDISMODULE_EVENT_LOADING &&
		   (subevent == REDISMODULE_SUBEVENT_LOADING_ENDED ||  // Load ended.
			subevent == REDISMODULE_SUBEVENT_LOADING_FAILED    // Load failed.
		   );
}

// Server persistence event handler.
static void _PersistenceEventHandler(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent,
									 void *data) {
	if(_IsEventPersistenceStart(eid, subevent)) _CreateKeySpaceMetaKeys(ctx);
	else if(_IsEventPersistenceEnd(eid, subevent)) _ClearKeySpaceMetaKeys(ctx);
}

// Server loading event handler.
static void _LoadingEventHandler(RedisModuleCtx *ctx, RedisModuleEvent eid, uint64_t subevent,
								 void *data) {
	if(_IsEventLoadingEnd(eid, subevent)) _ClearKeySpaceMetaKeys(ctx);
}

static void _RegisterServerEvents(RedisModuleCtx *ctx) {
	RedisModule_SubscribeToKeyspaceEvents(ctx, REDISMODULE_NOTIFY_GENERIC, _RenameGraphHandler);
	// Regiseter to server events for redis 6 and up.
	if(redis_major_version > 5) {
		RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_FlushDB, _FlushDBHandler);
		RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Persistence, _PersistenceEventHandler);
		RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Loading, _LoadingEventHandler);
	}
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

void RegisterEventHandlers(RedisModuleCtx *ctx) {
	_RegisterForkHooks();       // Set up hooks for forking logic to prevent bgsave deadlocks.
	_RegisterServerEvents(ctx); // Set up hooks renaming and server events on Redis 6 and up.
}
