#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

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
	int		e;	/* excess flow.			*/
	list_t*		edge;	/* adjacency list.		*/
	node_t*		next;	/* with excess preflow.		*/
    pthread_mutex_t mutex; /* mutex for node */
};

struct edge_t {
	node_t*		u;	/* one of the two nodes.	*/
	node_t*		v;	/* the other. 			*/
	int		f;	/* flow > 0 if from u to v.	*/
	int		c;	/* capacity.			*/
};

struct graph_t {
	int		n;	/* nodes.			*/
	int		m;	/* edges.			*/
	int		done;
	node_t*		v;	/* array of n nodes.		*/
	edge_t*		e;	/* array of m edges.		*/
	node_t*		s;	/* source.			*/
	node_t*		t;	/* sink.			*/
	node_t*		excess;	/* nodes with e > 0 except s,t.	*/
	cmd_list_t*  cmds;
	pthread_cond_t  cond;
	pthread_mutex_t mutex;
	pthread_barrier_t barrier;
};

struct command_t {
	int		push;
  int     d; // how much to push
	node_t*	u;
	node_t* v;
	edge_t* e;
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

static void push(graph_t* g, node_t* u, node_t* v, edge_t* e, int d)
{
	pr("push from %d to %d: ", id(g, u), id(g, v));
	pr("f = %d, c = %d, so ", e->f, e->c);
	pr("pushing %d\n", abs(d));


  u->e -= abs(d);
  v->e += abs(d);
  e->f += d;
  pr("  f = %d, c = %d, \n", e->f, e->c);
  pr("  node %d now e = %d\n", id(g, u), u->e);
  pr("  node %d now e = %d\n", id(g, v), v->e);

	/* the following are always true. */

	assert(u == g->s || u->e >= 0);
	assert(abs(e->f) <= e->c);
}

static void relabel(graph_t* g, node_t* u)
{
	pthread_mutex_lock(&u->mutex);
	u->h += 1;
	pthread_mutex_unlock(&u->mutex);

	pr("relabel %d now h = %d\n", id(g, u), u->h);
}

static node_t* other(node_t* u, edge_t* e)
{
	if (u == e->u)
		return e->v;
	else
		return e->u;
}

cmd_list_t* get_command(graph_t* g, node_t* u) // Previously dispatch
{
	node_t* v;
	edge_t* e;
	list_t* p;
	int		b, d;
  int remaining_excess = u->e;

	cmd_list_t* c_list = calloc(1, sizeof(cmd_list_t));
	pr("selected u = %d with ", id(g, u));
	pr("h = %d and e = %d\n", u->h, u->e);

	if (u->e == 0){
		free(c_list);
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

		if (remaining_excess == 0) {
			pr("No excess! Exit discharge.\n");
			break;
		}
		
		if (u->h > v->h && b * e->f < e->c) {
			pr("Sending push command\n");
			command_t* c = malloc(sizeof(command_t));
      if (u == e->u) {
        d = MIN(remaining_excess, e->c - e->f);
      } else {
        d = -MIN(remaining_excess, e->c + e->f);
      }
      remaining_excess -= abs(d);
      c->d = d;
			c->e = e;
			c->u = u;
			c->v = v;
			c->push = 1;
			c->next = c_list->head;
			c_list->head = c;
			if(c_list->tail == NULL){
				c_list->tail = c_list->head;
			}
		}
	}

	if (c_list->head == NULL){
		// Send relabel command
		command_t* c = calloc(1, sizeof(command_t));
		pr("Sending relabel command\n");
		c->push = 0;
		c->u = u;
		c_list->head = c;
		c_list->tail = c;
	}


	return c_list;
}

void execute(graph_t* g, command_t* c)
{
	if (!c->push) {
		pr("Executing relabel for u=%d\n", id(g, c->u));
		relabel(g, c->u);
	} else {
		pr("Executing push for u=%d (e=%d), v=%d\n", id(g, c->u), c->u->e, id(g, c->v));
		push(g, c->u, c->v, c->e, c->d);
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
		pr("Fas 1\n");

		int i;
		for (i = start; i <= stop; i++) {
			node_t* n = &g->v[i];
			cmd_list_t* new_c = get_command(g, n);

			
			if (new_c != NULL) {
				pthread_mutex_lock(&g->mutex);
				if(g->cmds != NULL){
					new_c->tail->next = g->cmds->head;
					g->cmds->head = new_c->head;
					free(new_c);
				}else{
					g->cmds= new_c;
				}
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
		if (g->cmds == NULL) {
			g->done = 1;
			pthread_barrier_wait(&g->barrier);
			continue;
		}

		command_t* c = g->cmds->head;
		
		while (g->cmds != NULL && c != NULL) {
			execute(g, c);
			c = c->next;
		}
		c = g->cmds->head;
		while (g->cmds != NULL && c != NULL) {
			command_t* temp = c;
			c = c->next;
			free(temp);
		}
		free(g->cmds);
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

	while (p != NULL) {
		e = p->edge;
		p = p->next;

		push(g, s, other(s, e), e, e->c);
	}

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

int smain(int argc, char* argv[])
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
