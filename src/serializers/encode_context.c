/*
* Copyright 2018-2020 Redis Labs Ltd. and Contributors
*
* This file is available under the Redis Labs Source Available License Agreement
*/

#include "encode_context.h"
#include "assert.h"
#include "../util/rmalloc.h"
#include "../util/rax_extensions.h"

inline GraphEncodeContext *GraphEncodeContext_New() {
	GraphEncodeContext *ctx = rm_malloc(sizeof(GraphEncodeContext));
	ctx->keys = raxNew();
	GraphEncodeContext_Reset(ctx);
	return ctx;
}

inline void GraphEncodeContext_Reset(GraphEncodeContext *ctx) {
	assert(ctx);
	ctx->phase = RESET;
	ctx->keys_processed = 0;
	ctx->processed_nodes = 0;
	ctx->processed_edges = 0;
	ctx->processed_deleted_nodes = 0;
	ctx->processed_deleted_edges = 0;
	ctx->datablock_iterator = NULL;
	ctx->current_relation_matrix_id = 0;
	ctx->matrix_tuple_iterator = NULL;
}

inline EncodePhase GraphEncodeContext_GetEncodePhase(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->phase;
}

inline void GraphEncodeContext_SetEncodePhase(GraphEncodeContext *ctx, EncodePhase phase) {
	assert(ctx);
	ctx->phase = phase;
}

inline uint64_t GraphEncodeContext_GetKeyCount(const GraphEncodeContext *ctx) {
	assert(ctx);
	return raxSize(ctx->keys) + 1;
}

inline uint64_t GraphEncodeContext_GetProcessedKeyCount(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->keys_processed;
}

inline uint64_t GraphEncodeContext_GetProcessedNodesCount(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->processed_nodes;
}

inline void GraphEncodeContext_SetProcessedNodesCount(GraphEncodeContext *ctx, uint64_t nodes) {
	assert(ctx);
	ctx->processed_nodes = nodes;
}

inline uint64_t GraphEncodeContext_GetProcessedDeletedNodesCount(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->processed_deleted_nodes;
}

inline void GraphEncodeContext_SetProcessedDeletedNodesCount(GraphEncodeContext *ctx,
															 uint64_t deleted_nodes) {
	assert(ctx);
	ctx->processed_deleted_nodes = deleted_nodes;
}

inline uint64_t GraphEncodeContext_GetProcessedEdgesCount(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->processed_edges;
}

inline void GraphEncodeContext_SetProcessedEdgesCount(GraphEncodeContext *ctx, uint64_t edges) {
	assert(ctx);
	ctx->processed_edges = edges;
}

inline uint64_t GraphEncodeContext_GetProcessedDeletedEdgesCount(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->processed_deleted_edges;
}

inline void GraphEncodeContext_SetProcessedDeletedEdgesCount(GraphEncodeContext *ctx,
															 uint64_t deleted_edges) {
	assert(ctx);
	ctx->processed_deleted_edges = deleted_edges;
}

inline DataBlockIterator *GraphEncodeContext_GetDatablockIterator(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->datablock_iterator;
}

inline void GraphEncodeContext_SetDatablockIterator(GraphEncodeContext *ctx,
													DataBlockIterator *iter) {
	assert(ctx);
	ctx->datablock_iterator = iter;
}

inline uint GraphEncodeContex_GetCurrentRelationID(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->current_relation_matrix_id;
}

inline void GraphEncodeContex_SetCurrentRelationID(GraphEncodeContext *ctx,
												   uint current_relation_matrix_id) {
	assert(ctx);
	ctx->current_relation_matrix_id = current_relation_matrix_id;
}

inline GxB_MatrixTupleIter *GraphEncodeContext_GetMatrixTupleIterator(
	const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->matrix_tuple_iterator;
}

inline void GraphEncodeContext_SetMatrixTupleIterator(GraphEncodeContext *ctx,
													  GxB_MatrixTupleIter *iter) {
	assert(ctx);
	ctx->matrix_tuple_iterator = iter;
}

inline bool GraphEncodeContext_Finished(const GraphEncodeContext *ctx) {
	assert(ctx);
	return ctx->keys_processed == GraphEncodeContext_GetKeyCount(ctx);
}

inline void GraphEncodeContext_AddKey(GraphEncodeContext *ctx, const char *key) {
	assert(ctx && key);
	raxInsert(ctx->keys, (unsigned char *)key, strlen(key), NULL, NULL);
}

inline void GraphEncodeContext_DeleteKey(GraphEncodeContext *ctx, const char *key) {
	assert(ctx);
	raxRemove(ctx->keys, (unsigned char *)key, strlen(key), NULL);
}

inline void GraphEncodeContext_IncreaseProcessedCount(GraphEncodeContext *ctx) {
	assert(ctx);
	assert(ctx->keys_processed < GraphEncodeContext_GetKeyCount(ctx));
	ctx->keys_processed++;
}

inline unsigned char **GraphEncodeContext_GetKeys(GraphEncodeContext *ctx) {
	assert(ctx);
	return raxKeys(ctx->keys);
}

inline void GraphEncodeContext_Free(GraphEncodeContext *ctx) {
	if(ctx) {
		if(ctx->keys) {
			raxFree(ctx->keys);
			ctx->keys = NULL;
		}
		rm_free(ctx);
	}
}