#define _ATFILE_SOURCE
#include "updater.h"
#include "graph.h"
#include "fileio.h"
#include "debug.h"
#include "db.h"
#include "parser.h"
#include "server.h"
#include "file.h"
#include "fslurp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/wait.h>

static int process_create_nodes(void);
static int process_update_nodes(void);
static int build_graph(struct graph *g);
static int add_file_cb(void *arg, struct db_node *dbn);
static int find_deps(struct graph *g, struct node *n);
static int execute_graph(struct graph *g, int keep_going,
			 void *(*work_func)(void *));
static void show_progress(int n, int tot);

static void *create_work(void *arg);
static void *update_work(void *arg);
static int update(struct node *n, struct server *s);
static int var_replace(struct node *n);
static int delete_file(struct node *n);

static int do_show_progress;
static int do_keep_going;

/*static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;*/
static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;

struct worker_thread {
	struct list_head list;
	pthread_t pid;
	int node_pipe[2];
	int status_fd;
	struct node *n;
	struct graph *g; /* This should only be used in create_work() */
};


int updater(int argc, char **argv)
{
	int x;

	do_show_progress = tup_db_config_get_int("show_progress");
	do_keep_going = tup_db_config_get_int("keep_going");

	for(x=1; x<argc; x++) {
		if(strcmp(argv[x], "-d") == 0) {
			debug_enable("tup.updater");
		} else if(strcmp(argv[x], "--show-progress") == 0) {
			do_show_progress = 1;
		} else if(strcmp(argv[x], "--no-show-progress") == 0) {
			do_show_progress = 0;
		} else if(strcmp(argv[x], "--keep-going") == 0 ||
			  strcmp(argv[x], "-k") == 0) {
			do_keep_going = 1;
		} else if(strcmp(argv[x], "--no-keep-going") == 0) {
			do_keep_going = 0;
		}
	}

	if(server_init() < 0)
		return -1;
	if(process_create_nodes() < 0)
		return -1;
	if(process_update_nodes() < 0)
		return -1;
	return 0;
}

static int process_create_nodes(void)
{
	struct graph g;
	int rc;

	if(create_graph(&g, TUP_NODE_DIR) < 0)
		return -1;
	if(tup_db_select_node_by_flags(add_file_cb, &g, TUP_FLAGS_CREATE) < 0)
		return -1;
	if(build_graph(&g) < 0)
		return -1;
	tup_db_begin();
	rc = execute_graph(&g, 0, create_work);
	if(rc == 0) {
		tup_db_commit();
	} else {
		tup_db_rollback();
		return -1;
	}
	if(destroy_graph(&g) < 0)
		return -1;
	return 0;
}

static int process_update_nodes(void)
{
	struct graph g;
	int rc;

	if(create_graph(&g, TUP_NODE_CMD) < 0)
		return -1;
	if(tup_db_select_node_by_flags(add_file_cb, &g, TUP_FLAGS_MODIFY) < 0)
		return -1;
	if(tup_db_select_node_by_flags(add_file_cb, &g, TUP_FLAGS_DELETE) < 0)
		return -1;
	if(build_graph(&g) < 0)
		return -1;
	tup_db_begin();
	rc = execute_graph(&g, do_keep_going, update_work);
	tup_db_commit();
	if(rc < 0)
		return -1;
	if(destroy_graph(&g) < 0)
		return -1;
	return 0;
}

static int build_graph(struct graph *g)
{
	struct node *cur;

	while(!list_empty(&g->plist)) {
		cur = list_entry(g->plist.next, struct node, list);
		if(cur->state == STATE_INITIALIZED) {
			DEBUGP("find deps for node: %lli\n", cur->tupid);
			if(find_deps(g, cur) < 0)
				return -1;
			cur->state = STATE_PROCESSING;
		} else if(cur->state == STATE_PROCESSING) {
			DEBUGP("remove node from stack: %lli\n", cur->tupid);
			list_del(&cur->list);
			list_add_tail(&cur->list, &g->node_list);
			cur->state = STATE_FINISHED;
		}
	}

	return 0;
}

static int add_file_cb(void *arg, struct db_node *dbn)
{
	struct graph *g = arg;
	struct node *n;

	if(find_node(g, dbn->tupid, &n) < 0)
		return -1;
	if(n != NULL)
		goto edge_create;
	n = create_node(g, dbn);
	if(!n)
		return -1;
	DEBUGP("create node: %lli (0x%x)\n", dbn->tupid, dbn->type);

edge_create:
	if(n->state == STATE_PROCESSING) {
		fprintf(stderr, "Error: Circular dependency detected! "
			"Last edge was: %lli -> %lli\n",
			g->cur->tupid, dbn->tupid);
		return -1;
	}
	if(create_edge(g->cur, n) < 0)
		return -1;
	return 0;
}

static int find_deps(struct graph *g, struct node *n)
{
	g->cur = n;
	if(tup_db_select_node_by_link(add_file_cb, g, n->tupid) < 0)
		return -1;
	return 0;
}

static void pop_node(struct graph *g, struct node *n)
{
	while(n->edges) {
		struct edge *e;
		e = n->edges;
		if(e->dest->state != STATE_PROCESSING) {
			list_del(&e->dest->list);
			list_add(&e->dest->list, &g->plist);
			e->dest->state = STATE_PROCESSING;
		}

		n->edges = remove_edge(e);
	}
}

static int execute_graph(struct graph *g, int keep_going,
			 void *(*work_func)(void *))
{
	struct node *root;
	struct worker_thread *wt;
	int num_processed = 0;
	int status_pipe[2];
	int rc = -1;

	if(g->num_nodes)
		printf("Parsing Tupfiles\n");

	wt = malloc(sizeof *wt);
	if(!wt) {
		perror("malloc");
		return -1;
	}
	if(pipe(status_pipe) < 0) {
		perror("pipe");
		return -1;
	}
	wt->g = g;
	wt->status_fd = status_pipe[1];
	if(pipe(wt->node_pipe) < 0) {
		perror("pipe");
		return -1;
	}
	if(pthread_create(&wt->pid, NULL, work_func, wt) < 0) {
		perror("pthread_create");
		return -1;
	}

	root = list_entry(g->node_list.next, struct node, list);
	DEBUGP("root node: %lli\n", root->tupid);
	pop_node(g, root);
	remove_node(g, root);

	show_progress(num_processed, g->num_nodes);
	while(!list_empty(&g->plist)) {
		struct node *n;
		char c = 1;
		n = list_entry(g->plist.next, struct node, list);
		DEBUGP("cur node: %lli [%i]\n", n->tupid, n->incoming_count);
		if(n->incoming_count) {
			list_move(&n->list, &g->node_list);
			n->state = STATE_FINISHED;
			continue;
		}
		wt->n = n;
		if(write(wt->node_pipe[1], &c, 1) != 1) {
			perror("write");
			return -1;
		}

		{
			int ret;
			int status;
			fd_set rfds;

			FD_ZERO(&rfds);
			FD_SET(status_pipe[0], &rfds);
			ret = select(status_pipe[0]+1, &rfds, NULL, NULL, NULL);
			if(ret <= 0) {
				perror("select");
				return -1;
			}
			pthread_mutex_lock(&status_mutex);
			read(status_pipe[0], &status, sizeof(status));
			pthread_mutex_unlock(&status_mutex);
			if(status < 0) {
				if(keep_going)
					goto keep_going;
				goto out;
			}
		}
		pop_node(g, n);

keep_going:
		if(n->type == g->count_flags && ! (n->flags&TUP_FLAGS_DELETE)) {
			num_processed++;
			show_progress(num_processed, g->num_nodes);
		}
		remove_node(g, n);
	}
	if(!list_empty(&g->node_list) || !list_empty(&g->plist)) {
		printf("\n");
		if(do_keep_going) {
			fprintf(stderr, "Remaining nodes skipped due to errors in command execution.\n");
		} else {
			fprintf(stderr, "Error: Graph is not empty after execution.\n");
		}
		goto out;
	}
	rc = 0;
out:
	{
		char c = 0;
		write(wt->node_pipe[1], &c, 1);
		pthread_join(wt->pid, NULL);
		close(wt->node_pipe[0]);
		close(wt->node_pipe[1]);
		free(wt);
	}
	close(status_pipe[0]);
	close(status_pipe[1]);
	return rc;
}

static void *create_work(void *arg)
{
	struct worker_thread *wt = arg;
	struct graph *g = wt->g;
	char c;
	while(read(wt->node_pipe[0], &c, 1) == 1) {
		struct node *n;
		int rc = 0;

		if(c == 0)
			break;
		n = wt->n;

		if(n->type == TUP_NODE_DIR) {
			if(n->already_used) {
				printf("Already parsed[%lli]: '%s'\n", n->tupid, n->name);
				rc = 0;
			} else {
				rc = parse(n, g);
			}
		} else if(n->type == TUP_NODE_VAR ||
			  n->type==TUP_NODE_FILE ||
			  n->type==TUP_NODE_CMD) {
			rc = 0;
		} else {
			fprintf(stderr, "Error: Unknown node %lli named '%s' in create graph.\n", n->tupid, n->name);
			rc = -1;
		}
		if(tup_db_unflag_create(n->tupid) < 0)
			rc = -1;

		pthread_mutex_lock(&status_mutex);
		write(wt->status_fd, &rc, sizeof(rc));
		pthread_mutex_unlock(&status_mutex);
	}
	return NULL;
}

static void *update_work(void *arg)
{
	struct worker_thread *wt = arg;
	struct server *s;
	char c;

	s = malloc(sizeof *s);
	if(!s) {
		perror("malloc");
		return NULL;
	}
	while(read(wt->node_pipe[0], &c, 1) == 1) {
		struct node *n;
		struct edge *e;
		int rc = 0;

		if(c == 0)
			break;
		n = wt->n;

		if(n->type == TUP_NODE_FILE &&
		   (n->flags == TUP_FLAGS_DELETE)) {
			printf("[35mDelete[%lli]: %s[0m\n",
			       n->tupid, n->name);
			rc = delete_file(n);
		} else if((n->type == TUP_NODE_DIR ||
			   n->type == TUP_NODE_VAR) &&
			  n->flags == TUP_FLAGS_DELETE) {
			printf("[35mDelete[%lli]: %s[0m\n",
			       n->tupid, n->name);
			rc = delete_name_file(n->tupid);

		} else if(n->type == TUP_NODE_CMD) {
			if(n->flags & TUP_FLAGS_DELETE) {
				printf("[35mDelete[%lli]: %s[0m\n",
				       n->tupid, n->name);
				rc = delete_name_file(n->tupid);
			} else {
				rc = update(n, s);
			}
		}

		if(rc == 0) {
			e = n->edges;
			while(e) {
				/* Mark the next nodes as modify in case we hit
				 * an error - we'll need to pick up there.
				 */
				if(tup_db_add_modify_list(e->dest->tupid) < 0)
					rc = -1;
				e = e->next;
			}
			if(tup_db_set_flags_by_id(n->tupid, TUP_FLAGS_NONE) < 0)
				rc = -1;
		}

		pthread_mutex_lock(&status_mutex);
		write(wt->status_fd, &rc, sizeof(rc));
		pthread_mutex_unlock(&status_mutex);
	}
	free(s);
	return NULL;
}

static int update(struct node *n, struct server *s)
{
	int status;
	int pid;
	int dfd = -1;
	int curfd = -1;
	int print_name = 1;
	const char *name = n->name;
	tupid_t tupid;

	/* Commands that begin with a ',' are special var/sed commands */
	if(name[0] == ',')
		return var_replace(n);

	tupid = tup_db_create_dup_node(n->dt, n->name, n->type);
	if(tupid < 0)
		return -1;

	if(name[0] == '@') {
		print_name = 0;
		name++;
	}

	curfd = open(".", O_RDONLY);
	if(curfd < 0)
			goto err_delete_node;

	dfd = tup_db_open_tupid(n->dt);
	if(dfd < 0)
		goto err_close_curfd;
	fchdir(dfd);

	if(print_name)
		printf("[%lli:%lli] %s\n", n->tupid, tupid, name);

	if(start_server(s) < 0) {
		fprintf(stderr, "Error starting update server.\n");
		goto err_close_dfd;
	}
	pid = fork();
	if(pid < 0) {
		perror("fork");
		goto err_close_dfd;
	}
	if(pid == 0) {
		execl("/bin/sh", "/bin/sh", "-c", name, NULL);
		perror("execl");
		exit(1);
	}
	wait(&status);
	if(stop_server(s) < 0)
		goto err_cmd_failed;

	if(WIFEXITED(status)) {
		if(WEXITSTATUS(status) == 0) {
			if(write_files(tupid, name, &s->finfo) < 0)
				goto err_cmd_failed;
		} else {
			goto err_cmd_failed;
		}
	}
	fchdir(curfd);

	close(dfd);
	close(curfd);
	delete_name_file(n->tupid);
	return 0;

err_cmd_failed:
	fprintf(stderr, " *** Command %lli failed.\n", n->tupid);
err_close_dfd:
	fchdir(curfd);
	close(dfd);
err_close_curfd:
	close(curfd);
err_delete_node:
	delete_name_file(tupid);
	return -1;
}

static int var_replace(struct node *n)
{
	int dfd;
	int curfd;
	int ifd;
	int ofd;
	struct buf b;
	char *input;
	char *rbracket;
	char *p, *e;
	int rc = -1;

	if(n->name[0] != ',') {
		fprintf(stderr, "Error: var_replace command must begin with ','\n");
		return -1;
	}
	input = n->name + 1;
	while(isspace(*input))
		input++;

	curfd = open(".", O_RDONLY);
	if(curfd < 0)
		return -1;

	dfd = tup_db_open_tupid(n->dt);
	if(dfd < 0)
		goto err_close_curfd;
	fchdir(dfd);

	printf("%s\n", input);
	rbracket = strchr(input, '>');
	if(rbracket == NULL) {
		fprintf(stderr, "Unable to find '>' in var/sed command '%s'\n",
			input);
		goto err_close_dfd;
	}
	/* Use -1 since the string is '%s > %s' and we need to set the space
	 * before the '>' to 0.
	 */
	if(rbracket == input) {
		fprintf(stderr, "Error: the '>' symbol can't be at the start of the var/sed command.\n");
		return -1;
	}
	rbracket[-1] = 0;

	ifd = open(input, O_RDONLY);
	if(ifd < 0) {
		perror(input);
		goto err_close_dfd;
	}
	if(fslurp(ifd, &b) < 0) {
		goto err_close_ifd;
	}
	ofd = creat(rbracket+2, 0666);
	if(ofd < 0) {
		perror(rbracket+2);
		goto err_free_buf;
	}

	p = b.s;
	e = b.s + b.len;
	do {
		char *at;
		char *rat;
		at = p;
		while(at < e && *at != '@') {
			at++;
		}
		if(write(ofd, p, at-p) != at-p) {
			perror("write");
			goto err_close_ofd;
		}
		if(at >= e)
			break;

		p = at;
		rat = p+1;
		while(rat < e && (isalnum(*rat) || *rat == '_')) {
			rat++;
		}
		if(rat < e && *rat == '@') {
			tupid_t varid;
			varid = tup_db_write_var(p+1, rat-(p+1), ofd);
			if(varid < 0)
				return -1;
			if(tup_db_create_link(varid, n->tupid) < 0)
				return -1;
			p = rat + 1;
		} else {
			if(write(ofd, p, rat-p) != rat-p) {
				perror("write");
				goto err_close_ofd;
			}
			p = rat;
		}
		
	} while(p < e);

	rc = 0;

err_close_ofd:
	close(ofd);
err_free_buf:
	free(b.s);
err_close_ifd:
	close(ifd);
err_close_dfd:
	fchdir(curfd);
	close(dfd);
err_close_curfd:
	close(curfd);
	return rc;
}

static int delete_file(struct node *n)
{
	int dirfd;
	int rc = 0;

	if(delete_name_file(n->tupid) < 0)
		return -1;
	dirfd = tup_db_open_tupid(n->dt);
	if(dirfd < 0) {
		if(dirfd == -ENOENT) {
			/* If the directory doesn't exist, the file can't
			 * either
			 */
			return 0;
		} else {
			return -1;
		}
	}

	if(unlinkat(dirfd, n->name, 0) < 0) {
		/* Don't care if the file is already gone. */
		if(errno != ENOENT) {
			perror(n->name);
			rc = -1;
			goto out;
		}
	}

out:
	close(dirfd);
	return rc;
}

static void show_progress(int n, int tot)
{
	if(do_show_progress && tot) {
		int x, a, b;
		const int max = 40;
		char c = '=';
		if(tot > max) {
			a = n * max / tot;
			b = max;
			c = '#';
		} else {
			a = n;
			b = tot;
		}
		printf("[");
		for(x=0; x<a; x++) {
			printf("%c", c);
		}
		for(x=a; x<b; x++) {
			printf(" ");
		}
		printf("] %i/%i (%3i%%) ", n, tot, n*100/tot);
		if(n == tot)
			printf("\n");
	}
}
