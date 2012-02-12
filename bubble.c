#include <limits.h>
#include "priv.h"
#include "mog.h"
#include "kvec.h"
#include "ksw.h"

#define edge_mark_del(_x) ((_x).x = (uint64_t)-2, (_x).y = 0)
#define edge_is_del(_x)   ((_x).x == (uint64_t)-2 || (_x).y == 0)

/******************
 * Closed bubbles *
 ******************/

typedef struct {
	uint32_t id;
	int cnt[2];
	int n[2][2], d[2][2];
	uint32_t v[2][2];
} trinfo_t;

typedef struct {
	int n, m;
	trinfo_t **buf;
} tipool_t;

struct mogb_aux {
	tipool_t pool;
	ku64_v stack;
};

mogb_aux_t *mog_b_initaux(void)
{
	return calloc(1, sizeof(mogb_aux_t));
}

void mog_b_destroyaux(mogb_aux_t *b)
{
	int i;
	for (i = 0; i < b->pool.n; ++i)
		free(b->pool.buf[i]);
	free(b->pool.buf); free(b->stack.a);
}

#define tiptr(p) ((trinfo_t*)(p)->ptr)

static inline trinfo_t *tip_alloc(tipool_t *pool, uint32_t id)
{
	trinfo_t *p;
	if (pool->n == pool->m) {
		int i, new_m = pool->m? pool->m<<1 : 256;
		pool->buf = realloc(pool->buf, new_m);
		for (i = pool->m; i < new_m; ++i)
			pool->buf[i] = malloc(sizeof(trinfo_t));
	}
	p = pool->buf[pool->n++];
	memset(p, 0, sizeof(trinfo_t));
	p->id = id;
	return p;
}

void mog_vh_pop_closed(mog_t *g, uint64_t idd, int max_vtx, int max_dist, mogb_aux_t *a)
{
	int i, n_pending = 0;
	mogv_t *p, *q;

	a->stack.n = a->pool.n = 0;
	p = &g->v.a[idd>>1];
	if (p->len < 0 || p->nei[idd&1].n < 2) return; // stop if p is deleted or it has 0 or 1 neighbor
	p->ptr = tip_alloc(&a->pool, idd>>1);
	tiptr(p)->d[(idd&1)^1][0] = tiptr(p)->d[(idd&1)^1][1] = -p->len;
	kv_push(uint64_t, a->stack, idd^1);
	while (a->stack.n) {
		uint64_t x, y;
		ku128_v *r;
		if (a->stack.n == 1 && a->stack.a[0] != (idd^1) && n_pending == 0) break; // found the other end of the bubble
		x = kv_pop(a->stack);
		p = &g->v.a[x>>1];
		printf("%lld:%lld\n", p->k[0], p->k[1]);
		r = &p->nei[(x&1)^1]; // we will look the the neighbors from the other end of the unitig
		if (a->stack.n > max_vtx || tiptr(p)->d[x&1][0] > max_dist || tiptr(p)->d[x&1][1] > max_dist || r->n == 0) break; // we failed
		// set the distance to p's neighbors
		for (i = 0; i < r->n; ++i) {
			int nsr;
			if (edge_is_del(r->a[i])) continue;
			y = mog_tid2idd(g->h, r->a[i].x);
			q = &g->v.a[y>>1];
			if (q->ptr == 0) { // has not been attempted
				q->ptr = tip_alloc(&a->pool, y>>1), ++n_pending;
				mog_v128_clean(&q->nei[y&1]); // make sure there are no deleted edges
			}
			nsr = tiptr(p)->n[x&1][0] + q->nsr;
			// test and possibly update the tentative distance
			if (nsr > tiptr(q)->n[y&1][0]) { // then move the best to the 2nd best and update the best
				tiptr(q)->n[y&1][1] = tiptr(q)->n[y&1][0]; tiptr(q)->n[y&1][0] = nsr;
				tiptr(q)->v[y&1][1] = tiptr(q)->v[y&1][0]; tiptr(q)->v[y&1][0] = x;
				tiptr(q)->d[y&1][1] = tiptr(q)->d[y&1][0]; tiptr(q)->d[y&1][0] = tiptr(p)->d[x&1][0] + p->len - r->a[i].y;
				nsr = tiptr(p)->n[x&1][1] + q->nsr; // now nsr is the 2nd best
			}
			if (nsr > tiptr(q)->n[y&1][1]) { // update the 2nd best
				tiptr(q)->n[y&1][1] = nsr, tiptr(q)->v[y&1][1] = x;
				tiptr(q)->d[y&1][1] = tiptr(p)->d[x&1][1] + p->len - r->a[i].y;
			}
			if (++tiptr(q)->cnt[y&1] == q->nei[y&1].n) { // all q's predecessors have been processed; then push
				kv_push(uint64_t, a->stack, y);
				--n_pending;
			}
		}
	}
	printf("%d\n", n_pending);
	for (i = 0; i < a->pool.n; ++i) // reset p->ptr
		g->v.a[a->pool.buf[i]->id].ptr = 0;
}

/****************
 * Open bubbles *
 ****************/

void mog_v_swrm(mog_t *g, mogv_t *p, int min_elen)
{
	int i, j, k, l, dir, max_l, l_qry;
	mogv_t *q, *t;
	ku128_v *r, *s;
	uint8_t *seq;
	int8_t mat[16];
	ksw_query_t *qry;
	ksw_aux_t aux;

	if (p->len < 0 || p->len >= min_elen) return;
	//if (p->nei[0].n && p->nei[1].n) return; // FIXME: between this and the next line, which is better?
	if (p->nei[0].n + p->nei[1].n != 1) return;
	dir = p->nei[0].n? 0 : 1;
	// initialize the scoring system
	for (i = k = 0; i < 4; ++i)
		for (j = 0; j < 4; ++j)
			mat[k++] = i == j? 5 : -4;
	aux.gapo = 6; aux.gape = 3;
	
	s = &p->nei[dir];
	for (l = 0; l < s->n; ++l) { // if we use "if (p->nei[0].n + p->nei[1].n != 1)", s->n == 1
		uint64_t v;
		if ((int64_t)s->a[l].x < 0) continue;
		v = mog_tid2idd(g->h, s->a[l].x);
		q = &g->v.a[v>>1];
		if (q == p || q->nei[v&1].n == 1) continue;
		// get the query ready
		max_l = (p->len - s->a[l].y) * 2;
		seq = malloc(max_l + 1);
		if (dir == 0) { // forward strand
			for (j = s->a[l].y, k = 0; j < p->len; ++j)
				seq[k++] = p->seq[j] - 1;
		} else { // reverse
			for (j = p->len - s->a[l].y - 1, k = 0; j >= 0; --j)
				seq[k++] = 4 - p->seq[j];
		}
		l_qry = k; aux.T = l_qry * 5 / 2;
		qry = ksw_qinit(2, l_qry, seq, 4, mat);
		//fprintf(stderr, "===> %lld:%lld:%d[%d], %d, %ld <===\n", p->k[0], p->k[1], s->n, l, p->nsr, q->nei[v&1].n);
		//for (j = 0; j < k; ++j) fputc("ACGTN"[(int)seq[j]], stderr); fputc('\n', stderr);

		r = &q->nei[v&1];
		for (i = 0; i < r->n; ++i) {
			uint64_t w;
			if (r->a[i].x == p->k[dir] || (int64_t)r->a[i].x < 0) continue;
			w = mog_tid2idd(g->h, r->a[i].x);
			// get the target sequence
			t = &g->v.a[w>>1];
			if (w&1) { // reverse strand
				for (j = t->len - r->a[i].y - 1, k = 0; j >= 0 && k < max_l; --j)
					seq[k++] = 4 - t->seq[j];
			} else {
				for (j = r->a[i].y, k = 0; j < t->len && k < max_l; ++j)
					seq[k++] = t->seq[j] - 1;
			}
			ksw_sse2(qry, k, seq, &aux);
			//for (j = 0; j < k; ++j) fputc("ACGTN"[(int)seq[j]], stderr); fprintf(stderr, "\t%d\t%f\n", aux.score, (l_qry * 5. - aux.score) / (5. + 4.));
			if (aux.score) {
				double r_diff, n_diff;
				n_diff = (l_qry * 5. - aux.score) / (5. + 4.); // 5: matching score; -4: mismatchig score
				r_diff = n_diff / l_qry;
				if (n_diff < 2.01 || r_diff < 0.1) break;
			}
		}

		if (i != r->n) {
			// mark delete in p and delete in q
			edge_mark_del(s->a[l]);
			for (i = 0; i < r->n; ++i)
				if (r->a[i].x == p->k[dir])
					edge_mark_del(r->a[i]);
		}
		free(seq); free(qry);
	}

	for (i = 0; i < s->n; ++i)
		if (!edge_is_del(s->a[i])) break;
	if (i == s->n) mog_v_del(g, p); // p is not connected to any other vertices
}

