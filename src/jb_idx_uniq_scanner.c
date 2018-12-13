#include "ejdb2_internal.h"

static_assert(IW_VNUMBUFSZ <= JBNUMBUF_SIZE, "IW_VNUMBUFSZ <= JBNUMBUF_SIZE");

static iwrc jb_idx_consume_eq(struct _JBEXEC *ctx, const JQVAL *rval, JB_SCAN_CONSUMER consumer) {
  uint64_t id;
  int64_t step;
  size_t sz;
  bool matched;
  struct _JBMIDX *midx = &ctx->midx;
  char buf[JBNUMBUF_SIZE];
  IWKV_val key = {.data = buf};

  jb_idx_jqval_fill_key(rval, &key);
  if (!key.size) {
    return IW_ERROR_ASSERTION;
  }
  iwrc rc = iwkv_get_copy(midx->idx->idb, &key, buf, sizeof(buf), &sz);
  if (rc) {
    if (rc == IWKV_ERROR_NOTFOUND) {
      return consumer(ctx, 0, 0, 0, 0, 0);
    } else {
      return rc;
    }
  }
  IW_READVNUMBUF64_2(buf, id);
  rc = consumer(ctx, 0, id, &step, &matched, 0);
  return consumer(ctx, 0, 0, 0, 0, rc);
}

static iwrc jb_idx_consume_in_binn(struct _JBEXEC *ctx, JQVAL *rval, JB_SCAN_CONSUMER consumer) {
  // todo
  return IW_ERROR_NOT_IMPLEMENTED;
}

static iwrc jb_idx_consume_in_node(struct _JBEXEC *ctx, JQVAL *rval, JB_SCAN_CONSUMER consumer) {
  // todo
  return IW_ERROR_NOT_IMPLEMENTED;
}

static iwrc jb_idx_consume_scan(struct _JBEXEC *ctx, JQVAL *rval, JB_SCAN_CONSUMER consumer) {
  size_t sz;
  bool matched;
  IWKV_cursor cur;
  int64_t step = 1;
  char buf[JBNUMBUF_SIZE];
  struct _JBMIDX *midx = &ctx->midx;
  JBIDX idx = midx->idx;
  IWKV_cursor_op cursor_reverse_step = (midx->cursor_step == IWKV_CURSOR_NEXT)
                                       ? IWKV_CURSOR_PREV : IWKV_CURSOR_NEXT;
  IWKV_val key = {.data = buf};
  jb_idx_jqval_fill_key(rval, &key);

  iwrc rc = iwkv_cursor_open(idx->idb, &cur, midx->cursor_init, &key);
  if (rc == IWKV_ERROR_NOTFOUND
      && (midx->expr1->op->value == JQP_OP_LT || midx->expr1->op->value == JQP_OP_LTE)) {
    iwkv_cursor_close(&cur);
    midx->cursor_init = IWKV_CURSOR_BEFORE_FIRST;
    rc = iwkv_cursor_open(idx->idb, &cur, midx->cursor_init, 0);
    RCGO(rc, finish);
  }
  if (midx->cursor_init < IWKV_CURSOR_NEXT) { // IWKV_CURSOR_BEFORE_FIRST || IWKV_CURSOR_AFTER_LAST
    rc = iwkv_cursor_to(cur, midx->cursor_step);
    RCGO(rc, finish);
  }
  do {
    if (step > 0) --step;
    else if (step < 0) ++step;
    if (!step) {
      uint64_t id;
      rc = iwkv_cursor_copy_val(cur, &buf, IW_VNUMBUFSZ, &sz);
      RCGO(rc, finish);
      if (sz > IW_VNUMBUFSZ) {
        rc = IWKV_ERROR_CORRUPTED;
        iwlog_ecode_error3(rc);
        break;
      }
      IW_READVNUMBUF64_2(buf, id);
      if (midx->expr2
          && !jb_idx_node_expr_matched(ctx->ux->q->qp->aux, midx->idx, cur, midx->expr2, &rc)) {
        break;
      }
      do {
        step = 1;
        matched = false;
        rc = consumer(ctx, 0, id, &step, &matched, 0);
        RCGO(rc, finish);
        if (!midx->expr1->prematched && matched) {
          // Further scan will always match main index expression
          midx->expr1->prematched = true;
        }
      } while (step < 0 && !++step);
    }
  } while (step && !(rc = iwkv_cursor_to(cur, step > 0 ? midx->cursor_step : cursor_reverse_step)));

finish:
  if (rc == IWKV_ERROR_NOTFOUND) rc = 0;
  if (cur) {
    iwkv_cursor_close(&cur);
  }
  return consumer(ctx, 0, 0, 0, 0, rc);
}

iwrc jb_idx_uniq_scanner(struct _JBEXEC *ctx, JB_SCAN_CONSUMER consumer) {
  iwrc rc;
  JQP_QUERY *qp = ctx->ux->q->qp;
  struct _JBMIDX *midx = &ctx->midx;
  JQVAL *rval = jql_unit_to_jqval(qp->aux, midx->expr1->right, &rc);
  RCRET(rc);
  switch (midx->expr1->op->value) {
    case JQP_OP_EQ:
      return jb_idx_consume_eq(ctx, rval, consumer);
    case JQP_OP_IN:
      if (rval->type == JQVAL_BINN) {
        return jb_idx_consume_in_binn(ctx, rval, consumer);
      } else if (rval->type == JQVAL_JBLNODE) {
        return jb_idx_consume_in_node(ctx, rval, consumer);
      } else {
        return IW_ERROR_ASSERTION;
      }
      break;
    default:
      break;
  }
  return jb_idx_consume_scan(ctx, rval, consumer);
}