/*
 * Builtin "git am"
 *
 * Based on git-am.sh by Junio C Hamano.
 */
#include "cache.h"
#include "builtin.h"
#include "exec_cmd.h"
#include "parse-options.h"
#include "dir.h"
#include "run-command.h"
#include "quote.h"
#include "cache-tree.h"
#include "refs.h"
#include "commit.h"

/**
 * Returns 1 if the file is empty or does not exist, 0 otherwise.
 */
static int is_empty_file(const char *filename)
{
	struct stat st;

	if (stat(filename, &st) < 0) {
		if (errno == ENOENT)
			return 1;
		die_errno(_("could not stat %s"), filename);
	}

	return !st.st_size;
}

/**
 * Returns the first line of msg
 */
static const char *firstline(const char *msg)
{
	static struct strbuf sb = STRBUF_INIT;

	strbuf_reset(&sb);
	strbuf_add(&sb, msg, strchrnul(msg, '\n') - msg);
	return sb.buf;
}

enum patch_format {
	PATCH_FORMAT_UNKNOWN = 0,
	PATCH_FORMAT_MBOX
};

struct am_state {
	/* state directory path */
	struct strbuf dir;

	/* current and last patch numbers, 1-indexed */
	int cur;
	int last;

	/* commit message and metadata */
	struct strbuf author_name;
	struct strbuf author_email;
	struct strbuf author_date;
	struct strbuf msg;

	/* number of digits in patch filename */
	int prec;
};

/**
 * Initializes am_state with the default values.
 */
static void am_state_init(struct am_state *state)
{
	memset(state, 0, sizeof(*state));

	strbuf_init(&state->dir, 0);
	strbuf_init(&state->author_name, 0);
	strbuf_init(&state->author_email, 0);
	strbuf_init(&state->author_date, 0);
	strbuf_init(&state->msg, 0);
	state->prec = 4;
}

/**
 * Release memory allocated by an am_state.
 */
static void am_state_release(struct am_state *state)
{
	strbuf_release(&state->dir);
	strbuf_release(&state->author_name);
	strbuf_release(&state->author_email);
	strbuf_release(&state->author_date);
	strbuf_release(&state->msg);
}

/**
 * Returns path relative to the am_state directory.
 */
static inline const char *am_path(const struct am_state *state, const char *path)
{
	return mkpath("%s/%s", state->dir.buf, path);
}

/**
 * Returns 1 if there is an am session in progress, 0 otherwise.
 */
static int am_in_progress(const struct am_state *state)
{
	struct stat st;

	if (lstat(state->dir.buf, &st) < 0 || !S_ISDIR(st.st_mode))
		return 0;
	if (lstat(am_path(state, "last"), &st) || !S_ISREG(st.st_mode))
		return 0;
	if (lstat(am_path(state, "next"), &st) || !S_ISREG(st.st_mode))
		return 0;
	return 1;
}

/**
 * Reads the contents of `file`. The third argument can be used to give a hint
 * about the file size, to avoid reallocs. Returns number of bytes read on
 * success, -1 if the file does not exist. If trim is set, trailing whitespace
 * will be removed from the file contents.
 */
static int read_state_file(struct strbuf *sb, const char *file, size_t hint, int trim)
{
	strbuf_reset(sb);
	if (strbuf_read_file(sb, file, hint) >= 0) {
		if (trim)
			strbuf_rtrim(sb);

		return sb->len;
	}

	if (errno == ENOENT)
		return -1;

	die_errno(_("could not read '%s'"), file);
}

/**
 * Parses the "author script" `filename`, and sets state->author_name,
 * state->author_email and state->author_date accordingly. We are strict with
 * our parsing, as the author script is supposed to be eval'd, and loosely
 * parsing it may not give the results the user expects.
 *
 * The author script is of the format:
 *
 * 	GIT_AUTHOR_NAME='$author_name'
 * 	GIT_AUTHOR_EMAIL='$author_email'
 * 	GIT_AUTHOR_DATE='$author_date'
 *
 * where $author_name, $author_email and $author_date are quoted.
 */
static int read_author_script(struct am_state *state)
{
	char *value;
	struct strbuf sb = STRBUF_INIT;
	const char *filename = am_path(state, "author-script");
	FILE *fp = fopen(filename, "r");
	if (!fp) {
		if (errno == ENOENT)
			return 0;
		die_errno(_("could not open '%s' for reading"), filename);
	}

	if (strbuf_getline(&sb, fp, '\n'))
		return -1;
	if (!skip_prefix(sb.buf, "GIT_AUTHOR_NAME=", (const char**) &value))
		return -1;
	value = sq_dequote(value);
	if (!value)
		return -1;
	strbuf_reset(&state->author_name);
	strbuf_addstr(&state->author_name, value);

	if (strbuf_getline(&sb, fp, '\n'))
		return -1;
	if (!skip_prefix(sb.buf, "GIT_AUTHOR_EMAIL=", (const char**) &value))
		return -1;
	value = sq_dequote(value);
	if (!value)
		return -1;
	strbuf_reset(&state->author_email);
	strbuf_addstr(&state->author_email, value);

	if (strbuf_getline(&sb, fp, '\n'))
		return -1;
	if (!skip_prefix(sb.buf, "GIT_AUTHOR_DATE=", (const char**) &value))
		return -1;
	value = sq_dequote(value);
	if (!value)
		return -1;
	strbuf_reset(&state->author_date);
	strbuf_addstr(&state->author_date, value);

	if (fgetc(fp) != EOF)
		return -1;

	fclose(fp);
	strbuf_release(&sb);
	return 0;
}

/**
 * Saves state->author_name, state->author_email and state->author_date in
 * `filename` as an "author script", which is the format used by git-am.sh.
 */
static void write_author_script(const struct am_state *state)
{
	static const char fmt[] = "GIT_AUTHOR_NAME=%s\n"
		"GIT_AUTHOR_EMAIL=%s\n"
		"GIT_AUTHOR_DATE=%s\n";
	struct strbuf author_name = STRBUF_INIT;
	struct strbuf author_email = STRBUF_INIT;
	struct strbuf author_date = STRBUF_INIT;

	sq_quote_buf(&author_name, state->author_name.buf);
	sq_quote_buf(&author_email, state->author_email.buf);
	sq_quote_buf(&author_date, state->author_date.buf);

	write_file(am_path(state, "author-script"), 1, fmt,
			author_name.buf, author_email.buf, author_date.buf);

	strbuf_release(&author_name);
	strbuf_release(&author_email);
	strbuf_release(&author_date);
}

/**
 * Loads state from disk.
 */
static void am_load(struct am_state *state)
{
	struct strbuf sb = STRBUF_INIT;

	read_state_file(&sb, am_path(state, "next"), 8, 1);
	state->cur = strtol(sb.buf, NULL, 10);

	read_state_file(&sb, am_path(state, "last"), 8, 1);
	state->last = strtol(sb.buf, NULL, 10);

	if (read_author_script(state) < 0)
		die(_("could not parse author script"));

	read_state_file(&state->msg, am_path(state, "final-commit"), 0, 0);

	strbuf_release(&sb);
}

/**
 * Remove the am_state directory.
 */
static void am_destroy(const struct am_state *state)
{
	struct strbuf sb = STRBUF_INIT;

	strbuf_addstr(&sb, state->dir.buf);
	remove_dir_recursively(&sb, 0);
	strbuf_release(&sb);
}

/*
 * Returns 1 if the file looks like a piece of email a-la RFC2822, 0 otherwise.
 * We check this by grabbing all the non-indented lines and seeing if they look
 * like they begin with valid header field names.
 */
static int is_email(const char *filename)
{
	struct strbuf sb = STRBUF_INIT;
	FILE *fp = xfopen(filename, "r");
	int ret = 1;

	while (!strbuf_getline(&sb, fp, '\n')) {
		const char *x;

		strbuf_rtrim(&sb);

		if (!sb.len)
			break; /* End of header */

		/* Ignore indented folded lines */
		if (*sb.buf == '\t' || *sb.buf == ' ')
			continue;

		/* It's a header if it matches the regexp "^[!-9;-~]+:" */
		for (x = sb.buf; *x; x++) {
			if (('!' <= *x && *x <= '9') || (';' <= *x && *x <= '~'))
				continue;
			if (*x == ':' && x != sb.buf)
				break;
			ret = 0;
			goto done;
		}
	}

done:
	fclose(fp);
	strbuf_release(&sb);
	return ret;
}

/**
 * Attempts to detect the patch_format of the patches contained in `paths`,
 * returning the PATCH_FORMAT_* enum value. Returns PATCH_FORMAT_UNKNOWN if
 * detection fails.
 */
static int detect_patch_format(struct string_list *paths)
{
	enum patch_format ret = PATCH_FORMAT_UNKNOWN;
	struct strbuf l1 = STRBUF_INIT;
	struct strbuf l2 = STRBUF_INIT;
	struct strbuf l3 = STRBUF_INIT;
	FILE *fp;

	/*
	 * We default to mbox format if input is from stdin and for directories
	 */
	if (!paths->nr || !strcmp(paths->items->string, "-") ||
	    is_directory(paths->items->string)) {
		ret = PATCH_FORMAT_MBOX;
		goto done;
	}

	/*
	 * Otherwise, check the first 3 lines of the first patch, starting
	 * from the first non-blank line, to try to detect its format.
	 */
	fp = xfopen(paths->items->string, "r");
	while (!strbuf_getline(&l1, fp, '\n')) {
		strbuf_trim(&l1);
		if (l1.len)
			break;
	}
	strbuf_getline(&l2, fp, '\n');
	strbuf_trim(&l2);
	strbuf_getline(&l3, fp, '\n');
	strbuf_trim(&l3);
	fclose(fp);

	if (starts_with(l1.buf, "From ") || starts_with(l1.buf, "From: "))
		ret = PATCH_FORMAT_MBOX;
	else if (l1.len && l2.len && l3.len && is_email(paths->items->string))
		ret = PATCH_FORMAT_MBOX;

done:
	strbuf_release(&l1);
	strbuf_release(&l2);
	strbuf_release(&l3);
	return ret;
}

/**
 * Splits out individual patches from `paths`, where each path is either a mbox
 * file or a Maildir. Return 0 on success, -1 on failure.
 */
static int split_patches_mbox(struct am_state *state, struct string_list *paths)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	struct strbuf last = STRBUF_INIT;

	cp.git_cmd = 1;
	argv_array_push(&cp.args, "mailsplit");
	argv_array_pushf(&cp.args, "-d%d", state->prec);
	argv_array_pushf(&cp.args, "-o%s", state->dir.buf);
	argv_array_push(&cp.args, "-b");
	argv_array_push(&cp.args, "--");

	for_each_string_list_item(item, paths)
		argv_array_push(&cp.args, item->string);

	if (capture_command(&cp, &last, 8))
		return -1;

	state->cur = 1;
	state->last = strtol(last.buf, NULL, 10);

	return 0;
}

/**
 * Splits out individual patches, of patch_format, contained within paths.
 * These patches will be stored in the state directory, with each patch's
 * filename being its index, padded to state->prec digits. state->cur will be
 * set to the index of the first patch, and state->last will be set to the
 * index of the last patch. Returns 0 on success, -1 on failure.
 */
static int split_patches(struct am_state *state, enum patch_format patch_format,
		struct string_list *paths)
{
	switch (patch_format) {
	case PATCH_FORMAT_MBOX:
		return split_patches_mbox(state, paths);
	default:
		die("BUG: invalid patch_format");
	}
	return -1;
}

/**
 * Setup a new am session for applying patches
 */
static void am_setup(struct am_state *state, enum patch_format patch_format,
		struct string_list *paths)
{
	if (!patch_format)
		patch_format = detect_patch_format(paths);

	if (!patch_format) {
		fprintf_ln(stderr, _("Patch format detection failed."));
		exit(128);
	}

	if (mkdir(state->dir.buf, 0777) < 0 && errno != EEXIST)
		die_errno(_("failed to create directory '%s'"), state->dir.buf);

	if (split_patches(state, patch_format, paths) < 0) {
		am_destroy(state);
		die(_("Failed to split patches."));
	}

	write_file(am_path(state, "next"), 1, "%d", state->cur);

	write_file(am_path(state, "last"), 1, "%d", state->last);
}

/**
 * Increments the patch pointer, and cleans am_state for the application of the
 * next patch.
 */
static void am_next(struct am_state *state)
{
	state->cur++;
	write_file(am_path(state, "next"), 1, "%d", state->cur);

	strbuf_reset(&state->author_name);
	strbuf_reset(&state->author_email);
	strbuf_reset(&state->author_date);
	unlink(am_path(state, "author-script"));

	strbuf_reset(&state->msg);
	unlink(am_path(state, "final-commit"));
}

/**
 * Returns the filename of the current patch.
 */
static const char *msgnum(const struct am_state *state)
{
	static struct strbuf sb = STRBUF_INIT;

	strbuf_reset(&sb);
	strbuf_addf(&sb, "%0*d", state->prec, state->cur);

	return sb.buf;
}

/**
 * Parses `patch` using git-mailinfo. state->msg will be set to the patch
 * message. state->author_name, state->author_email, state->author_date will be
 * set to the patch author's name, email and date respectively. The patch's
 * body will be written to "$state_dir/patch", where $state_dir is the state
 * directory.
 *
 * Returns 1 if the patch should be skipped, 0 otherwise.
 */
static int parse_patch(struct am_state *state, const char *patch)
{
	FILE *fp;
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf sb = STRBUF_INIT;

	cp.git_cmd = 1;
	cp.in = xopen(patch, O_RDONLY, 0);
	cp.out = xopen(am_path(state, "info"), O_WRONLY | O_CREAT, 0777);

	argv_array_push(&cp.args, "mailinfo");
	argv_array_push(&cp.args, am_path(state, "msg"));
	argv_array_push(&cp.args, am_path(state, "patch"));

	if (run_command(&cp) < 0)
		die("could not parse patch");

	close(cp.in);
	close(cp.out);

	/* Extract message and author information */
	fp = xfopen(am_path(state, "info"), "r");
	while (!strbuf_getline(&sb, fp, '\n')) {
		const char *x;

		if (skip_prefix(sb.buf, "Subject: ", &x)) {
			if (state->msg.len)
				strbuf_addch(&state->msg, '\n');
			strbuf_addstr(&state->msg, x);
		} else if (skip_prefix(sb.buf, "Author: ", &x)) {
			if (state->author_name.len)
				strbuf_addch(&state->author_name, '\n');
			strbuf_addstr(&state->author_name, x);
		} else if (skip_prefix(sb.buf, "Email: ", &x)) {
			if (state->author_email.len)
				strbuf_addch(&state->author_email, '\n');
			strbuf_addstr(&state->author_email, x);
		} else if (skip_prefix(sb.buf, "Date: ", &x)) {
			if (state->author_date.len)
				strbuf_addch(&state->author_date, '\n');
			strbuf_addstr(&state->author_date, x);
		}
	}
	fclose(fp);

	/* Skip pine's internal folder data */
	if (!strcmp(state->author_name.buf, "Mail System Internal Data"))
		return 1;

	if (is_empty_file(am_path(state, "patch")))
		die(_("Patch is empty. Was it split wrong?\n"
		"If you would prefer to skip this patch, instead run \"git am --skip\".\n"
		"To restore the original branch and stop patching run \"git am --abort\"."));

	strbuf_addstr(&state->msg, "\n\n");
	if (strbuf_read_file(&state->msg, am_path(state, "msg"), 0) < 0)
		die_errno(_("could not read '%s'"), am_path(state, "msg"));
	stripspace(&state->msg, 0);

	return 0;
}

/*
 * Applies current patch with git-apply. Returns 0 on success, -1 otherwise.
 */
static int run_apply(const struct am_state *state)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	cp.git_cmd = 1;

	argv_array_push(&cp.args, "apply");
	argv_array_push(&cp.args, "--index");
	argv_array_push(&cp.args, am_path(state, "patch"));

	if (run_command(&cp))
		return -1;

	/* Reload index as git-apply will have modified it. */
	discard_cache();
	read_cache();

	return 0;
}

/**
 * Commits the current index with state->msg as the commit message and
 * state->author_name, state->author_email and state->author_date as the author
 * information.
 */
static void do_commit(const struct am_state *state)
{
	unsigned char tree[20], parent[20], commit[20];
	unsigned char *ptr;
	struct commit_list *parents = NULL;
	const char *reflog_msg, *author;
	struct strbuf sb = STRBUF_INIT;

	if (write_cache_as_tree(tree, 0, NULL))
		die(_("git write-tree failed to write a tree"));

	if (!get_sha1_commit("HEAD", parent)) {
		ptr = parent;
		commit_list_insert(lookup_commit(parent), &parents);
	} else {
		ptr = NULL;
		fprintf_ln(stderr, _("applying to an empty history"));
	}

	author = fmt_ident(state->author_name.buf, state->author_email.buf,
			state->author_date.buf, IDENT_STRICT);

	if (commit_tree(state->msg.buf, state->msg.len, tree, parents, commit,
				author, NULL))
		die(_("failed to write commit object"));

	reflog_msg = getenv("GIT_REFLOG_ACTION");
	if (!reflog_msg)
		reflog_msg = "am";

	strbuf_addf(&sb, "%s: %s", reflog_msg, firstline(state->msg.buf));

	update_ref(sb.buf, "HEAD", commit, ptr, 0, UPDATE_REFS_DIE_ON_ERR);

	strbuf_release(&sb);
}

/**
 * Applies all queued patches.
 */
static void am_run(struct am_state *state)
{
	while (state->cur <= state->last) {
		const char *patch = am_path(state, msgnum(state));

		if (!file_exists(patch))
			goto next;

		if (parse_patch(state, patch))
			goto next; /* patch should be skipped */

		write_author_script(state);
		write_file(am_path(state, "final-commit"), 1, "%s", state->msg.buf);

		printf_ln(_("Applying: %s"), firstline(state->msg.buf));

		if (run_apply(state) < 0) {
			int value;

			printf_ln(_("Patch failed at %s %s"), msgnum(state),
					firstline(state->msg.buf));

			if (!git_config_get_bool("advice.amworkdir", &value) && !value)
				printf_ln(_("The copy of the patch that failed is found in: %s"),
						am_path(state, "patch"));

			exit(128);
		}

		do_commit(state);

next:
		am_next(state);
	}

	am_destroy(state);
}

/**
 * parse_options() callback that validates and sets opt->value to the
 * PATCH_FORMAT_* enum value corresponding to `arg`.
 */
static int parse_opt_patchformat(const struct option *opt, const char *arg, int unset)
{
	int *opt_value = opt->value;

	if (!strcmp(arg, "mbox"))
		*opt_value = PATCH_FORMAT_MBOX;
	else
		return -1;
	return 0;
}

static struct am_state state;
static int opt_patch_format;

static const char * const am_usage[] = {
	N_("git am [options] [(<mbox>|<Maildir>)...]"),
	NULL
};

static struct option am_options[] = {
	OPT_CALLBACK(0, "patch-format", &opt_patch_format, N_("format"),
		N_("format the patch(es) are in"), parse_opt_patchformat),
	OPT_END()
};

int cmd_am(int argc, const char **argv, const char *prefix)
{
	if (!getenv("_GIT_USE_BUILTIN_AM")) {
		const char *path = mkpath("%s/git-am", git_exec_path());

		if (sane_execvp(path, (char**) argv) < 0)
			die_errno("could not exec %s", path);
	}

	git_config(git_default_config, NULL);

	am_state_init(&state);
	strbuf_addstr(&state.dir, git_path("rebase-apply"));

	argc = parse_options(argc, argv, prefix, am_options, am_usage, 0);

	if (am_in_progress(&state))
		am_load(&state);
	else {
		struct string_list paths = STRING_LIST_INIT_DUP;
		int i;

		for (i = 0; i < argc; i++) {
			if (is_absolute_path(argv[i]) || !prefix)
				string_list_append(&paths, argv[i]);
			else
				string_list_append(&paths, mkpath("%s/%s", prefix, argv[i]));
		}

		am_setup(&state, opt_patch_format, &paths);

		string_list_clear(&paths, 0);
	}

	am_run(&state);

	am_state_release(&state);

	return 0;
}
