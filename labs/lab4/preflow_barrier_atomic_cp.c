#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#define PRINT		0	/* enable/disable prints. */

#if PRINT
#define pr(...)		do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define pr(...)		/* no effect at all */
#endif

#define MIN(a,b)	(((a)<=(b))?(a):(b))
#define MAX(a,b)	(((a)<=(b))?(b):(a))

typedef struct graph_t	graph_t;
typedef struct node_t	node_t;
typedef struct edge_t	edge_t;
typedef struct list_t	list_t;
typedef struct command_t command_t;
typedef struct args_t   args_t;
typedef struct cmd_list_t cmd_list_t;

struct list_t {
	edge_t*		edge;
	list_t*		next;
};

struct node_t {
	int		h;	/* height.			*/
	atomic_int		e;	/* excess flow.			*/
	atomic_int temp_e;
	list_t*		edge;	/* adjacency list.		*/
	node_t*		next;	/* with excess preflow.		*/
    pthread_mutex_t mutex; /* mutex for node */

};

struct edge_t {
	node_t*		u;	/* one of the two nodes.	*/
	node_t*		v;	/* the other. 			*/
	atomic_int		f;	/* flow > 0 if from u to v.	*/
	int		c;	/* capacity.			*/
};

struct graph_t {
	int		n;	/* nodes.			*/
	int		m;	/* edges.			*/
	int		done;
	atomic_int 	pushed_last;
	node_t*		v;	/* array of n nodes.		*/
	edge_t*		e;	/* array of m edges.		*/
	node_t*		s;	/* source.			*/
	node_t*		t;	/* sink.			*/
	node_t*		excess;	/* nodes with e > 0 except s,t.	*/
	command_t*  cmds;
	pthread_cond_t  cond;
	pthread_mutex_t mutex;
	pthread_barrier_t barrier;
};

struct command_t {
	node_t*	u;
	command_t* next;
};

struct cmd_list_t {
	command_t* tail;
	command_t* head;
};

struct args_t {
	graph_t* g;
	int start;
	int stop;
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

	/* memset sets everything (in this case) to 0. */
	memset(p, 0, n * s);

	return p;
}

static void add_edge(node_t* u, edge_t* e)
{
	list_t*		p;

	p = xmalloc(sizeof(list_t));
	p->edge = e;
	p->next = u->edge;
	u->edge = p;
}

static void connect(node_t* u, node_t* v, int c, edge_t* e)
{
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
	
	g->pushed_last = 0;

	pthread_mutex_init(&g->mutex, NULL);
	pthread_cond_init(&g->cond, NULL);
	
	g->v = xcalloc(n, sizeof(node_t));
	g->e = xcalloc(m, sizeof(edge_t));

	g->s = &g->v[0];
	g->t = &g->v[n-1];
	g->excess = NULL;
	g->cmds = NULL;

    for (i = 0; i < n; i += 1){
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

static void push(graph_t* g, node_t* u, node_t* v, edge_t* e)
{
	int		d;	/* remaining capacity of the edge. */

	

	if (u == e->u) {
		d = MIN(u->e, e->c - atomic_load_explicit(&e->f, memory_order_relaxed));
		
		pr("push from %d to %d: f = %d, c = %d, so pushing %d\n", id(g, u), id(g, v), e->f, e->c, d);
		//e->f += d;
		atomic_fetch_add_explicit(&e->f, d, memory_order_acq_rel);
	} else {
		d = MIN(u->e, e->c + atomic_load_explicit(&e->f, memory_order_relaxed));
		pr("push from %d to %d: f = %d, c = %d, so pushing %d\n", id(g, u), id(g, v), e->f, e->c, d);
		//e->f -= d;
		atomic_fetch_sub_explicit(&e->f, d, memory_order_acq_rel);
	}
	
	u->e -= d;
	v->e += d;

	/* the following are always true. */
	assert(d >= 0);
	assert(u->e >= 0);
	assert(abs(e->f) <= e->c);
}

static void relabel(graph_t* g, node_t* u)
{
	u->h += 1;

	pr("relabel %d now h = %d\n", id(g, u), u->h);
}

static node_t* other(node_t* u, edge_t* e)
{
	if (u == e->u)
		return e->v;
	else
		return e->u;
}

command_t* get_command(graph_t* g, node_t* u) // Previously dispatch
{
	node_t* v;
	edge_t* e;
	list_t* p;
	int		b, d;
	command_t* c = calloc(1, sizeof(command_t));

	pr("Sel u = %d h = %d, e = %d\n", id(g, u), u->h, u->e);

	if (u->e == 0){
		free(c);
		return NULL;
	}

	v = NULL;
	p = u->edge;

	while (p != NULL) {
		e = p->edge;
		p = p->next;

		if (u == e->u) {
			v = e->v;
			b = 1;
		} else {
			v = e->u;
			b = -1;
		}

		if (u->e == 0) {
			pr("No excess! Exit discharge.\n");
			break;
		}
		
		if (u->h > v->h && b * e->f < e->c) {
			pr("Sending push command\n");
			//pthread_mutex_lock(&g->mutex);
			push(g, u, v, e);
			g->pushed_last += 1;
			//pthread_mutex_unlock(&g->mutex);
		}
	}

	if (u->e != 0){
		// Send relabel command
		pr("Sending relabel command for node %d\n", id(g, u));
		c->u = u;
		return c;
	}else{
		free(c);
		return NULL;
	}

}

void* push_thread(void* arg)
{
	args_t*  args = (args_t*) arg;
	graph_t* g = args->g;
    node_t*  v;
	edge_t*  e;
	list_t*  p;
	int      b;
	int 	 start = args->start;
	int 	 stop = args->stop;
	free(args);
	while (!g->done) {
		// Fas 1
		//pr("Fas 1\n");

		int i;
		for (i = start; i <= stop; i++) {
			node_t* n = &g->v[i];
			command_t* new_c = get_command(g, n);
			
			if (new_c != NULL) {
				pthread_mutex_lock(&g->mutex);
				new_c->next = g->cmds;
				g->cmds = new_c;
				pthread_mutex_unlock(&g->mutex);
			}
		}

		int resp = pthread_barrier_wait(&g->barrier);

		if (resp == 0) {
			pthread_barrier_wait(&g->barrier);
			continue;
		}
		
		// Fas 2
		pr("Fas 2\n");
		if (g->cmds == NULL && g->pushed_last == 0) {
			g->done = 1;
			pthread_barrier_wait(&g->barrier);
			continue;
		}
		g->pushed_last = 0;
		command_t* c = g->cmds;
		
		while (c != NULL) {
			relabel(g, c->u);
			c = c->next;
		}
		c = g->cmds;
		while (c != NULL) {
			command_t* temp = c;
			c = c->next;
			free(temp);
		}

		g->cmds = NULL;

		pthread_barrier_wait(&g->barrier);
	};
}
	
int preflow(graph_t* g, int thread_amount)
{
	node_t*		s;
	node_t*		u;
	node_t*		v;
	edge_t*		e;
	list_t*		p;
 	pthread_t   threads[thread_amount];
	int			b, i;

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

	/* then loop until only s and/or t have excess preflow. */

	g->done = 0;

	int k = 1;
	int nodes_per_thread = (g->n - 2) / thread_amount;

	for (i = 0; i < thread_amount - 1; i++) {
		args_t* args = malloc(sizeof(args_t));
		args->g = g;
		args->start = k;
		args->stop = k + nodes_per_thread - 1;
		k += nodes_per_thread;

		pr("start: %d, stop: %d\n", args->start, args->stop);
		assert(args->start <= args->stop);
		// Initialisera här
		pthread_create(&threads[i], NULL, push_thread, args);
	}

	args_t* args = malloc(sizeof(args_t));
	args->g = g;
	args->start = k;
	args->stop = g->n - 2;
	pthread_create(&threads[i], NULL, push_thread, args);

	for (int i = 0; i < thread_amount; i++) {
		pthread_join(threads[i], NULL);
	}

	for (int i = 0; i < g->n; i++) {
		pr("@%d: e=%d, h=%d\n", id(g, &g->v[i]), g->v[i].e, g->v[i].h);
	}
	//free(args);
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

    // init_timebase();

	progname = argv[0];	/* name is a string in argv[0]. */

	in = stdin;		/* same as System.in in Java.	*/

	n = next_int();
	m = next_int();

	/* skip C and P from the 6railwayplanning lab in EDAF05 */
	next_int();
	next_int();

	g = new_graph(in, n, m);
	int thread_amount = MIN(g->n - 2, 7);
	pthread_barrier_init(&g->barrier, NULL, thread_amount);


	fclose(in);

	// double begin = timebase_sec();
	f = preflow(g, thread_amount);
	// double end = timebase_sec();

	// printf("t = %lf s\n", end-begin);
	printf("f = %d\n", f);

	free_graph(g);

	return 0;
}