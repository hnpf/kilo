#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "editor.h"

#define GIT_PATH_CAP 4096
#define GIT_BRANCH_CAP 128
#define GIT_MAX_LINES 1000000u
#define GIT_MAX_EDIT_DISTANCE 512

struct git_state {
	char *file;
	char root[GIT_PATH_CAP];
	char branch[GIT_BRANCH_CAP];
	char head_commit[48];
	uint64_t *head_hashes;
	size_t head_count;
	unsigned char *line_marks;
	unsigned char *delete_before;
	size_t mark_count;
	unsigned long diff_gen;
	time_t branch_checked;
	time_t repo_checked;
	int in_repo;
	int head_tracked;
	int head_exists;
	int head_loaded;
};

struct edit_op { int kind, at; };

static uint64_t hash_start(void) { return UINT64_C(14695981039346656037); }

static uint64_t hash_byte(uint64_t hash, unsigned char byte) {
	return (hash ^ byte) * UINT64_C(1099511628211);
}

static uint64_t hash_text(const char *text, size_t length) {
	uint64_t hash = hash_start();
	for (size_t i = 0; i < length; i++) hash = hash_byte(hash, (unsigned char)text[i]);
	return hash;
}

static int wait_child(pid_t child) {
	int status;
	while (waitpid(child, &status, 0) == -1) {
		if (errno == EINTR) continue;
		return 0;
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int capture_command(char *const argv[], char *output, size_t output_size) {
	int pipefd[2];
	if (!output || output_size < 2 || pipe(pipefd) == -1) return 0;
	pid_t child = fork();
	if (child == -1) { close(pipefd[0]); close(pipefd[1]); return 0; }
	if (child == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) == -1) _exit(127);
		close(pipefd[1]);
		int nullfd = open("/dev/null", O_WRONLY);
		if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	size_t used = 0;
	int too_long = 0;
	char chunk[512];
	ssize_t got;
	while ((got = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
		size_t available = output_size - used - 1;
		size_t copy = (size_t)got < available ? (size_t)got : available;
		if (copy) { memcpy(output + used, chunk, copy); used += copy; }
		if (copy < (size_t)got) too_long = 1;
	}
	close(pipefd[0]);
	output[used] = '\0';
	int success = wait_child(child) && !too_long && got == 0;
	while (used > 0 && (output[used - 1] == '\n' || output[used - 1] == '\r'))
		output[--used] = '\0';
	return success;
}

static int append_hash(uint64_t **hashes, size_t *count, size_t *capacity,
		uint64_t hash) {
	if (*count >= GIT_MAX_LINES) return 0;
	if (*count == *capacity) {
		size_t next = *capacity ? *capacity * 2 : 256;
		if (next > GIT_MAX_LINES) next = GIT_MAX_LINES;
		if (next <= *capacity || next > SIZE_MAX / sizeof(**hashes)) return 0;
		uint64_t *grown = realloc(*hashes, next * sizeof(**hashes));
		if (!grown) return 0;
		*hashes = grown;
		*capacity = next;
	}
	(*hashes)[(*count)++] = hash;
	return 1;
}

static int read_head_hashes(const char *root, const char *relative_path,
		uint64_t **result, size_t *result_count) {
	char spec[GIT_PATH_CAP + 8];
	if (snprintf(spec, sizeof(spec), "HEAD:%s", relative_path) >= (int)sizeof(spec)) return 0;
	char *argv[] = {"git", "-C", (char *)root, "show", spec, NULL};
	int pipefd[2];
	if (pipe(pipefd) == -1) return 0;
	pid_t child = fork();
	if (child == -1) { close(pipefd[0]); close(pipefd[1]); return 0; }
	if (child == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) == -1) _exit(127);
		close(pipefd[1]);
		int nullfd = open("/dev/null", O_WRONLY);
		if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pipefd[1]);
	uint64_t *hashes = NULL, hash = hash_start();
	size_t count = 0, capacity = 0, pending_cr = 0;
	int touched = 0, okay = 1;
	unsigned char chunk[8192];
	ssize_t got;
	while ((got = read(pipefd[0], chunk, sizeof(chunk))) > 0) {
		for (ssize_t i = 0; i < got; i++) {
			unsigned char byte = chunk[i];
			if (byte == '\n') {
				if (!append_hash(&hashes, &count, &capacity, hash)) okay = 0;
				hash = hash_start(); pending_cr = 0; touched = 0;
			} else {
				touched = 1;
				if (byte == '\r') pending_cr++;
				else {
				while (pending_cr) { hash = hash_byte(hash, '\r'); pending_cr--; }
				hash = hash_byte(hash, byte);
				}
			}
		}
	}
	close(pipefd[0]);
	if (got == 0 && touched && !append_hash(&hashes, &count, &capacity, hash)) okay = 0;
	int success = wait_child(child);
	if (!success || got == -1 || !okay) { free(hashes); return 0; }
	*result = hashes;
	*result_count = count;
	return 1;
}

static void clear_diff(struct git_state *state) {
	free(state->head_hashes); state->head_hashes = NULL; state->head_count = 0;
	free(state->line_marks); state->line_marks = NULL;
	free(state->delete_before); state->delete_before = NULL;
	state->mark_count = 0;
	state->head_tracked = 0;
	state->head_exists = 0;
	state->head_loaded = 0;
	state->diff_gen = ULONG_MAX;
}

static void make_file_directory(const char *file, char directory[GIT_PATH_CAP],
		char basename[GIT_PATH_CAP]) {
	char dir[GIT_PATH_CAP];
	if (!file || !*file) {
		if (!getcwd(dir, sizeof(dir))) return;
		basename[0] = '\0';
	} else {
		const char *slash = strrchr(file, '/');
		if (!slash) {
			snprintf(dir, sizeof(dir), ".");
			snprintf(basename, GIT_PATH_CAP, "%s", file);
		} else {
			size_t dlen = (size_t)(slash - file);
			if (dlen == 0) dlen = 1;
			if (dlen >= sizeof(dir)) return;
			memcpy(dir, file, dlen); dir[dlen] = '\0';
			snprintf(basename, GIT_PATH_CAP, "%s", slash + 1);
		}
	}
	if (!realpath(dir, directory)) directory[0] = '\0';
}

static int relative_to_root(const char *root, const char *directory,
		const char *basename, char relative[GIT_PATH_CAP]) {
	if (!basename || !*basename || !directory || !*directory) return 0;
	size_t root_len = strlen(root), dir_len = strlen(directory);
	const char *subdir = NULL;
	if (!strcmp(root, directory)) subdir = "";
	else if (root_len == 1 && root[0] == '/' && directory[0] == '/') subdir = directory + 1;
	else if (dir_len > root_len && !strncmp(root, directory, root_len) &&
		 directory[root_len] == '/') subdir = directory + root_len + 1;
	else return 0;
	int written = *subdir ? snprintf(relative, GIT_PATH_CAP, "%s/%s", subdir, basename) :
		snprintf(relative, GIT_PATH_CAP, "%s", basename);
	return written >= 0 && written < GIT_PATH_CAP;
}

static int head_file_tracked(const char *root, const char *relative_path) {
	char spec[GIT_PATH_CAP + 8], output[8];
	if (snprintf(spec, sizeof(spec), "HEAD:%s", relative_path) >= (int)sizeof(spec)) return 0;
	char *argv[] = {"git", "-C", (char *)root, "cat-file", "-e", spec, NULL};
	return capture_command(argv, output, sizeof(output));
}

static void load_head_file(struct git_state *state, const char *directory,
		const char *basename) {
	char relative[GIT_PATH_CAP];
	if (!relative_to_root(state->root, directory, basename, relative)) return;
	state->head_tracked = head_file_tracked(state->root, relative);
	if (state->head_tracked)
		state->head_exists = read_head_hashes(state->root, relative,
			&state->head_hashes, &state->head_count);
	state->head_loaded = 1;
}

static int refresh_branch(struct git_state *state) {
	if (!state->in_repo || !state->root[0]) return 0;
	char old_branch[GIT_BRANCH_CAP], old_commit[48];
	snprintf(old_branch, sizeof(old_branch), "%s", state->branch);
	snprintf(old_commit, sizeof(old_commit), "%s", state->head_commit);
	char branch[GIT_BRANCH_CAP];
	char *argv[] = {"git", "-C", state->root, "branch", "--show-current", NULL};
	if (!capture_command(argv, branch, sizeof(branch))) {
		state->branch_checked = time(NULL);
		return 0;
	}
	if (!branch[0]) {
		char *detached[] = {"git", "-C", state->root, "rev-parse", "--short", "HEAD", NULL};
		char commit[32];
		if (capture_command(detached, commit, sizeof(commit)) && commit[0])
			snprintf(branch, sizeof(branch), "@%s", commit);
		else snprintf(branch, sizeof(branch), "unborn");
	}
	for (size_t i = 0; branch[i]; i++)
		if ((unsigned char)branch[i] < 32 || (unsigned char)branch[i] >= 127) branch[i] = '?';
	snprintf(state->branch, sizeof(state->branch), "%s", branch);
	char commit[48] = "";
	char *head_argv[] = {"git", "-C", state->root, "rev-parse", "--short", "HEAD", NULL};
	capture_command(head_argv, commit, sizeof(commit));
	snprintf(state->head_commit, sizeof(state->head_commit), "%s", commit);
	state->branch_checked = time(NULL);
	int changed = strcmp(old_branch, state->branch) != 0;
	if (strcmp(old_commit, commit) != 0 && (old_commit[0] || commit[0])) changed = 1;
	if (changed) {
		free(state->head_hashes); state->head_hashes = NULL; state->head_count = 0;
		state->head_tracked = state->head_exists = state->head_loaded = 0;
		state->diff_gen = ULONG_MAX;
	}
	return changed;
}

static void load_repository(struct git_state *state) {
	clear_diff(state);
	state->in_repo = 0;
	state->root[0] = state->branch[0] = state->head_commit[0] = '\0';
	state->repo_checked = time(NULL);
	char directory[GIT_PATH_CAP] = {0}, basename[GIT_PATH_CAP] = {0};
	make_file_directory(g.fn, directory, basename);
	if (!directory[0]) return;
	char *root_argv[] = {"git", "-C", directory, "rev-parse", "--show-toplevel", NULL};
	if (!capture_command(root_argv, state->root, sizeof(state->root)) || !state->root[0]) return;
	state->in_repo = 1;
	refresh_branch(state);
	load_head_file(state, directory, basename);
}

static int trace_value(const int *trace, int layer, int diagonal) {
	int slot = (diagonal + layer) / 2;
	return trace[layer * (layer + 1) / 2 + slot];
}

static int mark_myers_region(const uint64_t *base, int base_start, int base_len,
		const uint64_t *current, int current_start, int current_len,
		unsigned char *marks, unsigned char *deletions) {
	int prefix = 0;
	while (prefix < base_len && prefix < current_len &&
		base[base_start + prefix] == current[current_start + prefix]) prefix++;
	int suffix = 0;
	while (suffix < base_len - prefix && suffix < current_len - prefix &&
		base[base_start + base_len - suffix - 1] == current[current_start + current_len - suffix - 1]) suffix++;
	int a_len = base_len - prefix - suffix;
	int b_len = current_len - prefix - suffix;
	if (!a_len && !b_len) return 1;
	int max_distance = a_len + b_len;
	int limit = max_distance < GIT_MAX_EDIT_DISTANCE ? max_distance : GIT_MAX_EDIT_DISTANCE;
	if (limit == 0) return 1;
	if (max_distance > (INT_MAX - 3) / 2 ||
		(size_t)max_distance > (SIZE_MAX / sizeof(int) - 3) / 2) goto fallback;
	int *v = malloc((size_t)(2 * max_distance + 3) * sizeof(*v));
	size_t trace_len = (size_t)(limit + 1) * (size_t)(limit + 2) / 2;
	int *trace = malloc(trace_len * sizeof(*trace));
	if (!v || !trace) { free(v); free(trace); goto fallback; }
	for (int i = 0; i < 2 * max_distance + 3; i++) v[i] = -1;
	int offset = max_distance + 1;
	v[offset + 1] = 0;
	int found = -1;
	for (int d = 0; d <= limit; d++) {
		for (int k = -d; k <= d; k += 2) {
			int x;
			if (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1]))
				x = v[offset + k + 1];
			else x = v[offset + k - 1] + 1;
			int y = x - k;
			while (x < a_len && y < b_len &&
				base[base_start + prefix + x] == current[current_start + prefix + y]) { x++; y++; }
			v[offset + k] = x;
			if (x >= a_len && y >= b_len) { found = d; break; }
		}
		for (int k = -d; k <= d; k += 2)
			trace[d * (d + 1) / 2 + (k + d) / 2] = v[offset + k];
		if (found >= 0) break;
	}
	if (found < 0) { free(v); free(trace); goto fallback; }
	struct edit_op ops[GIT_MAX_EDIT_DISTANCE * 2 + 2];
	int op_count = 0, split_pending = 0;
	int x = a_len, y = b_len;
	for (int d = found; d > 0; d--) {
		int k = x - y;
		int prev_k;
		if (k == -d || (k != d && trace_value(trace, d - 1, k - 1) < trace_value(trace, d - 1, k + 1)))
			prev_k = k + 1;
		else prev_k = k - 1;
		int prev_x = trace_value(trace, d - 1, prev_k);
		int prev_y = prev_x - prev_k;
		int had_snake = 0;
		while (x > prev_x && y > prev_y) { x--; y--; had_snake = 1; }
		if (had_snake && op_count > 0) split_pending = 1;
		if (split_pending && op_count > 0) {
			ops[op_count++] = (struct edit_op){0, 0};
			split_pending = 0;
		}
		if (x == prev_x) {
			y--;
			ops[op_count++] = (struct edit_op){1, current_start + prefix + y};
		} else {
			x--;
			ops[op_count++] = (struct edit_op){2, current_start + prefix + y};
		}
	}
	free(v); free(trace);
	for (int i = 0; i < op_count / 2; i++) {
		struct edit_op swap = ops[i]; ops[i] = ops[op_count - i - 1]; ops[op_count - i - 1] = swap;
	}
	for (int i = 0; i < op_count;) {
		if (ops[i].kind == 0) { i++; continue; }
		int end = i, insertions = 0, deletions_count = 0, first_delete = -1, last_insert = -1;
		while (end < op_count && ops[end].kind != 0) {
			if (ops[end].kind == 1) { insertions++; last_insert = ops[end].at; }
			else { deletions_count++; if (first_delete < 0) first_delete = ops[end].at; }
			end++;
		}
		int inserted = 0;
		for (int p = i; p < end; p++) if (ops[p].kind == 1) {
			int at = ops[p].at;
			if (at >= 0 && at < current_start + current_len)
				marks[at] = inserted < deletions_count ? '~' : '+';
			inserted++;
		}
		if (deletions_count > insertions) {
			int boundary = insertions ? last_insert + 1 : first_delete;
			if (boundary >= 0 && boundary <= current_start + current_len) deletions[boundary] = 1;
		}
		i = end;
	}
	return 1;

fallback:
	{
		int paired = a_len < b_len ? a_len : b_len;
		for (int i = 0; i < paired; i++) marks[current_start + prefix + i] = '~';
		for (int i = paired; i < b_len; i++) marks[current_start + prefix + i] = '+';
		if (a_len > b_len) deletions[current_start + prefix + b_len] = 1;
	}
	return 0;
}

static void build_markers(struct git_state *state) {
	free(state->line_marks); state->line_marks = NULL;
	free(state->delete_before); state->delete_before = NULL;
	state->mark_count = (size_t)(g.nr > 0 ? g.nr : 0);
	if (state->mark_count > GIT_MAX_LINES) { state->mark_count = 0; return; }
	state->line_marks = calloc(state->mark_count + 1, 1);
	state->delete_before = calloc(state->mark_count + 1, 1);
	if (!state->line_marks || !state->delete_before) {
		free(state->line_marks); free(state->delete_before);
		state->line_marks = state->delete_before = NULL;
		state->mark_count = 0;
		return;
	}
	if (!state->in_repo || !g.fn || !state->head_loaded) return;
	if (!state->head_tracked) {
		for (int i = 0; i < g.nr; i++) state->line_marks[i] = '+';
		return;
	}
	if (!state->head_exists) return;
	if ((size_t)g.nr > GIT_MAX_LINES) return;
	if ((size_t)g.nr > SIZE_MAX / sizeof(uint64_t)) return;
	uint64_t *current = malloc(sizeof(*current) * (size_t)(g.nr > 0 ? g.nr : 1));
	if (!current) return;
	for (int i = 0; i < g.nr; i++)
		current[i] = hash_text(g.r[i].s, (size_t)g.r[i].l);
	int head_count = state->head_count > INT_MAX ? INT_MAX : (int)state->head_count;
	mark_myers_region(state->head_hashes, 0, head_count, current, 0, g.nr,
		state->line_marks, state->delete_before);
	free(current);
}

struct git_state *git_state_create(void) {
	struct git_state *state = calloc(1, sizeof(*state));
	if (state) state->diff_gen = ULONG_MAX;
	return state;
}

void git_state_destroy(struct git_state *state) {
	if (!state) return;
	free(state->file);
	clear_diff(state);
	free(state);
}

void git_state_suspend(struct git_state *state) {
	if (!state) return;
	clear_diff(state);
}

void git_state_refresh(struct git_state *state, unsigned long buffer_gen) {
	if (!state) return;
	const char *file = g.fn ? g.fn : "";
	if (!state->file || strcmp(state->file, file)) {
		free(state->file);
		state->file = strdup(file);
		state->branch_checked = 0;
		load_repository(state);
	}
	else if (!state->in_repo && time(NULL) - state->repo_checked >= 2) load_repository(state);
	if (state->in_repo && time(NULL) - state->branch_checked >= 2) refresh_branch(state);
	if (state->in_repo && !state->head_loaded && g.fn) {
		char directory[GIT_PATH_CAP] = {0}, basename[GIT_PATH_CAP] = {0};
		make_file_directory(g.fn, directory, basename);
		load_head_file(state, directory, basename);
	}
	if (state->diff_gen != buffer_gen) {
		build_markers(state);
		state->diff_gen = buffer_gen;
	}
}

char git_line_marker(const struct git_state *state, int line) {
	if (!state || !state->line_marks || !state->delete_before || line < 0 ||
		(size_t)line > state->mark_count) return 0;
	if ((size_t)line < state->mark_count && state->line_marks[line]) return (char)state->line_marks[line];
	return state->delete_before[line] ? '-' : 0;
}

const char *git_branch_name(const struct git_state *state) {
	return g.git_enabled && state ? state->branch : "";
}

int git_state_enabled(const struct git_state *state) {
	return g.git_enabled && state && state->in_repo;
}
