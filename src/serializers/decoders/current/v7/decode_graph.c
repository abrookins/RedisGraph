/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "decode_v7.h"

// Module event handler functions declarations.
void ModuleEventHandler_IncreaseDecodingGraphsCount(void);
void ModuleEventHandler_DecreaseDecodingGraphsCount(void);

static GraphContext *_GetOrCreateGraphContext(char *graph_name) {

	GraphContext *gc = GraphContext_GetRegisteredGraphContext(graph_name);
	if(!gc) {
		// New graph is being decoded. Inform the module and create new graph context.
		ModuleEventHandler_IncreaseDecodingGraphsCount();
		gc = GraphContext_New(graph_name, GRAPH_DEFAULT_NODE_CAP, GRAPH_DEFAULT_EDGE_CAP);
		// While loading the graph, minimize matrix realloc and synchronization calls.
		Graph_SetMatrixPolicy(gc->g, RESIZE_TO_CAPACITY);
	}
	// Free the name string, as it either not in used or copied.
	RedisModule_Free(graph_name);
	// Set the thread-local GraphContext, as it will be accessed if we're decoding indexes.
	QueryCtx_SetGraphCtx(gc);
	return gc;
}

/* The first initialization of the graph data structure guarantees that there will be no further re-allocation
 * of data blocks and matrices since they are all in the appropriate size. */
static void _InitGraphDataStructure(Graph *g, uint64_t node_count, uint64_t edge_count,
									uint64_t label_count,  uint64_t relation_count) {
	DataBlock_Accommodate(g->nodes, node_count);
	DataBlock_Accommodate(g->edges, edge_count);
	for(uint64_t i = 0; i < label_count; i++) Graph_AddLabel(g);
	for(uint64_t i = 0; i < relation_count; i++) Graph_AddRelationType(g);
}

static GraphContext *_DecodeHeader(RedisModuleIO *rdb) {
	/* Header format:
	 * Graph name
	 * Node count
	 * Edge count
	 * Label matrix count
	 * Relation matrix count
	 * Number of graph keys (graph context key + meta keys)
	 */

	// Graph name
	char *graph_name = RedisModule_LoadStringBuffer(rdb, NULL);

	// Each key header contains the following: #nodes, #edges, #labels matrices, #relation matrices
	uint64_t node_count = RedisModule_LoadUnsigned(rdb);
	uint64_t edge_count = RedisModule_LoadUnsigned(rdb);
	uint64_t label_count = RedisModule_LoadUnsigned(rdb);
	uint64_t relation_count = RedisModule_LoadUnsigned(rdb);

	// Total keys representing the graph.
	uint64_t key_number = RedisModule_LoadUnsigned(rdb);

	GraphContext *gc = _GetOrCreateGraphContext(graph_name);
	// If it is the first key of this graph, allocate all the data structures, with the appropriate dimensions.
	if(GraphDecodeContext_GetProcessedKeyCount(gc->decoding_context) == 0) {
		_InitGraphDataStructure(gc->g, node_count, edge_count, label_count, relation_count);
	}
	GraphDecodeContext_SetKeyCount(gc->decoding_context, key_number);
	return gc;
}

GraphContext *RdbLoadGraph_v7(RedisModuleIO *rdb) {

	/* Key format:
	 * Header
	 * Payload(s)
	 * */

	GraphContext *gc = _DecodeHeader(rdb);
	EncodeState encoded_state = RedisModule_LoadUnsigned(rdb);
	/* The decode process contains the decode operation of many meta keys, representing independent parts of the graph.
	 * Each key contains data on one of the following:
	 * 1. Nodes - The nodes that are currently valid in the graph.
	 * 2. Deleted nodes - Nodes that were deleted and there ids can be re-used. Used for exact replication of data black state.
	 * 3. Edges - The edges that are currently valid in the graph.
	 * 4. Deleted edges - Edges that were deleted and there ids can be re-used. Used for exact replication of data black state.
	 * 5. Graph schema - Propertoes, indices.
	 * The following switch checks which part of the graph the current key holds, and decodes it accordingly. */
	switch(encoded_state) {
	case NODES:
		RdbLoadNodes_v7(rdb, gc);
		break;
	case DELETED_NODES:
		RdbLoadDeletedNodes_v7(rdb, gc);
		break;
	case EDGES:
		RdbLoadEdges_v7(rdb, gc);
		break;
	case DELETED_EDGES:
		RdbLoadDeletedEdges_v7(rdb, gc);
		break;
	case GRAPH_SCHEMA:
		RdbLoadGraphSchema_v7(rdb, gc);
		break;
	default:
		assert(false && "Unknown encoding");
		break;
	}
	GraphDecodeContext_IncreaseProcessedKeyCount(gc->decoding_context);
	if(GraphDecodeContext_Finished(gc->decoding_context)) {
		// Revert to default synchronization behavior
		Graph_SetMatrixPolicy(gc->g, SYNC_AND_MINIMIZE_SPACE);
		Graph_ApplyAllPending(gc->g);
		// Index the nodes when decoding ends.
		uint node_schemas_count = array_len(gc->node_schemas);
		for(uint i = 0; i < node_schemas_count; i++) {
			Schema *s = gc->node_schemas[i];
			if(s->index) Index_Construct(s->index);
			if(s->fulltextIdx) Index_Construct(s->fulltextIdx);
		}
		GraphDecodeContext_Reset(gc->decoding_context);
		// Graph has finished decoding, inform the module.
		ModuleEventHandler_DecreaseDecodingGraphsCount();
	}
	QueryCtx_Free(); // Release thread-local variables.
	return gc;
}
