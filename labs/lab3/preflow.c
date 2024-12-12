#include "timebase.h"
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREAD_COUNT 8

#define PRINT		0	/* enable/disable prints. */

#if PRINT
#define pr(...)		do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define pr(...)		/* no effect at all */
#endif

#define MIN(a,b)	(((a)<=(b))?(a):(b))

typedef struct graph_t	graph_t;
typedef struct node_t	node_t;
typedef struct edge_t	edge_t;
typedef struct list_t	list_t;
typedef struct command_t command_t;
typedef struct args_t   args_t;
struct list_t {
	edge_t*		edge;
	list_t*		next;
};

struct node_t {
	int		h;	/* height.			*/
	int		e;	/* excess flow.			*/
	list_t*		edge;	/* adjacency list.		*/
	node_t*		next;	/* with excess preflow.		*/
  pthread_mutex_t mutex;
};

struct edge_t {
	node_t*		u;	/* one of the two nodes.	*/
	node_t*		v;	/* the other. 			*/
	int		f;	/* flow > 0 if from u to v.	*/
	int		c;	/* capacity.			*/
  // mutex here?
};

struct graph_t {
	int		n;	/* nodes.			*/
	int		m;	/* edges.			*/
	node_t*		v;	/* array of n nodes.		*/
	edge_t*		e;	/* array of m edges.		*/
	node_t*		s;	/* source.			*/
	node_t*		t;	/* sink.			*/
	node_t*		excess;	/* nodes with e > 0 except s,t.	*/
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int active_threads;
};

static char* progname;

#if PRINT

static int id(graph_t* g, node_t* v)
{
	return v - g->v;
}
#endif

void error(const char* fmt, ...)
{
	va_list		ap;
	char		buf[BUFSIZ];

	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);

	if (progname != NULL)
		fprintf(stderr, "%s: ", progname);

	fprintf(stderr, "error: %s\n", buf);
	exit(1);
}

static int next_int()
{
        int     x;
        int     c;
	x = 0;
        while (isdigit(c = getchar()))
                x = 10 * x + c - '0';

        return x;
}

static void* xmalloc(size_t s)
{
	void*		p;
	p = malloc(s);

	if (p == NULL)
		error("out of memory: malloc(%zu) failed", s);

	return p;
}

static void* xcalloc(size_t n, size_t s)
{
	void*		p;

	p = xmalloc(n * s);

	memset(p, 0, n * s);

	return p;
}

static void add_edge(node_t* u, edge_t* e)
{
	list_t*		p;

	/* allocate memory for a list link and put it first
	 * in the adjacency list of u.
	 *
	 */

	p = xmalloc(sizeof(list_t));
	p->edge = e;
	p->next = u->edge;
	u->edge = p;
}

static void connect(node_t* u, node_t* v, int c, edge_t* e)
{
	/* connect two nodes by putting a shared (same object)
	 * in their adjacency lists.
	 *
	 */

	e->u = u;
	e->v = v;
	e->c = c;

	add_edge(u, e);
	add_edge(v, e);
}

static graph_t* new_graph(FILE* in, int n, int m)
{
	graph_t*	g;
	node_t*		u;
	node_t*		v;
	int		i;
	int		a;
	int		b;
	int		c;
	
	g = xmalloc(sizeof(graph_t));

	g->n = n;
	g->m = m;
	
	g->v = xcalloc(n, sizeof(node_t));
	g->e = xcalloc(m, sizeof(edge_t));

	pthread_mutex_init(&g->mutex, NULL);
	pthread_cond_init(&g->cond, NULL);
  g->active_threads = 0;

	g->s = &g->v[0];
	g->t = &g->v[n-1];
	g->excess = NULL;

  for (i = 0; i < n; i += 1) {
		pthread_mutex_init(&g->v[i].mutex, NULL);
	}

	for (i = 0; i < m; i += 1) {
		a = next_int();
		b = next_int();
		c = next_int();
		u = &g->v[a];
		v = &g->v[b];
		connect(u, v, c, g->e+i);
	}

	return g;
}

static void enter_excess(graph_t* g, node_t* v)
{
	/* put v at the front of the list of nodes
	 * that have excess preflow > 0.
	 *
	 * note that for the algorithm, this is just
	 * a set of nodes which has no order but putting it
	 * it first is simplest.
	 *
	 */
  pthread_mutex_lock(&g->mutex);
	if (v != g->t && v != g->s) {
    pr("Add node %d to excess list.\n", id(g,v));
		v->next = g->excess;
		g->excess = v;
	}
  pthread_mutex_unlock(&g->mutex);
}

static node_t* leave_excess(graph_t* g)
{
	node_t*		v;

	/* take any node from the set of nodes with excess preflow
	 * and for simplicity we always take the first.
	 *
	 */

	v = g->excess;

	if (v != NULL) {
    pr("Remove node %d from excess list.\n", id(g,v));
		g->excess = v->next;
  }

	return v;
}

static void push(graph_t* g, node_t* u, node_t* v, edge_t* e)
{
	int		d;	/* remaining capacity of the edge. */

	pr("push from %d to %d: ", id(g, u), id(g, v));
	pr("f = %d, c = %d, so ", e->f, e->c);
	
	if (u == e->u) {
		d = MIN(u->e, e->c - e->f);
		e->f += d;
	} else {
		d = MIN(u->e, e->c + e->f);
		e->f -= d;
	}

	pr("pushing %d\n", d);

	u->e -= d;
	v->e += d;

	/* the following are always true. */

	assert(d >= 0);
	assert(u->e >= 0);
	assert(abs(e->f) <= e->c);

	if (u->e > 0) {
    pr("Node %d still has excess.\n", id(g,u));

		/* still some remaining so let u push more. */

		/*enter_excess(g, u);*/
	}

	if (v->e == d) {
    pr("Node %d now has excess.\n", id(g,v));

		/* since v has d excess now it had zero before and
		 * can now push.
		 *
		 */

		enter_excess(g, v);
	}
}

static void relabel(graph_t* g, node_t* u)
{
  pthread_mutex_lock(&u->mutex);
	u->h += 1;
	pr("relabel %d now h = %d\n", id(g, u), u->h);
  pthread_mutex_unlock(&u->mutex);

	enter_excess(g, u);
}

static node_t* other(node_t* u, edge_t* e)
{
	if (u == e->u)
		return e->v;
	else
		return e->u;
}

void unlock_nodes(node_t* u, node_t* v) {
  if (u < v) {
    pthread_mutex_unlock(&u->mutex);
    pthread_mutex_unlock(&v->mutex);
  } else {
    pthread_mutex_unlock(&v->mutex);
    pthread_mutex_unlock(&u->mutex);
  }
}

void lock_nodes(node_t* u, node_t* v) {
  if (u < v) {
    pthread_mutex_lock(&u->mutex);
    pthread_mutex_lock(&v->mutex);
  } else {
    pthread_mutex_lock(&v->mutex);
    pthread_mutex_lock(&u->mutex);
  }
}

void discharge(graph_t* g, node_t* u) {
  list_t* neighbor = u->edge;
  int b; // direction of flow.
  node_t* v; // node to send to.
  edge_t* e;

	/* pr("Node %d discharge with ", id(g, u));*/
	/*pr("h = %d and e = %d\n", u->h, u->e);*/

  while (neighbor != NULL) {
    // find direction in order to calculate remaining capacity of edge.
    // lock mutex of nodes in correct order.
    // push if edge has capacity remaining.
    // remove edge from list.
    e = neighbor->edge;
    neighbor = neighbor->next;
    
    
    if (u == e->u) {
      b = 1;
      v = e->v;
    } else {
      b = -1;
      v = e->u;
    }

    lock_nodes(u, v); // lock nodes in same order every time.

    pr("Discharge node %d, e %d, h %d, to neighbor %d, h %d\n", id(g, u), u->e, u->h, id(g, v), v->h);
    
    if (u->e == 0) {
      unlock_nodes(u, v);
      pr("No excess! Exit discharge.\n");
      break;
    }
    if (u->h > v->h && e->f * b < e->c) {
      push(g, u, v, e);
    }
    
    unlock_nodes(u, v);
  }

  // List now empty, if excess remaining -> relabel and call discharge again.
  if (u->e > 0) {
    pr("Node %d excess remaining %d, relabel.\n", id(g,u), u->e);
    relabel(g, u);
    /*discharge(g, u);*/
  }
}

void* thread_main(void *arg) 
{
  node_t* u;
  graph_t *g = (graph_t *)arg;
	pr("Thread initialized.\n");
  /* Find node with excess, push until empty. */
  while (true) {
    pthread_mutex_lock(&g->mutex);
    // If no node has excess, wait until one has.
    while ((u = leave_excess(g)) == NULL) {
      // if no threads are active, terminate thread.
      pr("Thread has no node. Active Threads: %d\n", g->active_threads);
      if (g->active_threads == 0) {
        pr("Thread done.\n");
        pthread_mutex_unlock(&g->mutex);
        return (void*)NULL;
      }
      pthread_cond_wait(&g->cond, &g->mutex);
    }
    g->active_threads += 1;

    pr("Activating thread for node %d, now: %d\n", id(g,u), g->active_threads);
    pthread_mutex_unlock(&g->mutex);
    discharge(g, u);
    
    pthread_mutex_lock(&g->mutex);
    g->active_threads -= 1;
    pr("Deactivating thread, now: %d\n", g->active_threads);
    pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->mutex);
  }
}
	
int preflow(graph_t* g)
{
	node_t*		s;
	node_t*		u;
	node_t*		v;
	edge_t*		e;
	list_t*		p;
	int		    i;
  pthread_t threads[THREAD_COUNT];

	s = g->s;
	s->h = g->n;

	p = s->edge;

	/* start by pushing as much as possible (limited by
	 * the edge capacity) from the source to its neighbors.
	 *
	 */

	while (p != NULL) {
		e = p->edge;
		p = p->next;

		s->e += e->c;
		push(g, s, other(s, e), e);
	}
	/* create threads. */
  for (i = 0; i < THREAD_COUNT; i++) {
    pthread_create(&threads[i], NULL, thread_main, g);
  }
  /* wait for threads to be done. */
  for (i = 0; i < THREAD_COUNT; i++) {
    pthread_join(threads[i], NULL);
  }

	return g->t->e;
}

static void free_graph(graph_t* g)
{
	int		i;
	list_t*		p;
	list_t*		q;

	for (i = 0; i < g->n; i += 1) {
		p = g->v[i].edge;
		while (p != NULL) {
			q = p->next;
			free(p);
			p = q;
		}
	}
	free(g->v);
	free(g->e);
	free(g);
}

int main(int argc, char* argv[])
{
	FILE*		in;	/* input file set to stdin	*/
	graph_t*	g;	/* undirected graph. 		*/
	int		f;	/* output from preflow.		*/
	int		n;	/* number of nodes.		*/
	int		m;	/* number of edges.		*/

	progname = argv[0];	/* name is a string in argv[0]. */

	in = stdin;		/* same as System.in in Java.	*/

	n = next_int();
	m = next_int();

	/* skip C and P from the 6railwayplanning lab in EDAF05 */
	next_int();
	next_int();

	g = new_graph(in, n, m);

	fclose(in);

	f = preflow(g);

	printf("f = %d\n", f);

	free_graph(g);

	return 0;
}
