/* vi: set sw=4 ts=4: */
/*
 * A prototype Bourne shell grammar parser.
 * Intended to follow the original Thompson and Ritchie
 * "small and simple is beautiful" philosophy, which
 * incidentally is a good match to today's BusyBox.
 *
 * Copyright (C) 2000,2001  Larry Doolittle  <larry@doolittle.boa.org>
 * Copyright (C) 2008,2009  Denys Vlasenko <vda.linux@googlemail.com>
 *
 * Credits:
 *      The parser routines proper are all original material, first
 *      written Dec 2000 and Jan 2001 by Larry Doolittle.  The
 *      execution engine, the builtins, and much of the underlying
 *      support has been adapted from busybox-0.49pre's lash, which is
 *      Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
 *      written by Erik Andersen <andersen@codepoet.org>.  That, in turn,
 *      is based in part on ladsh.c, by Michael K. Johnson and Erik W.
 *      Troan, which they placed in the public domain.  I don't know
 *      how much of the Johnson/Troan code has survived the repeated
 *      rewrites.
 *
 * Other credits:
 *      o_addchr derived from similar w_addchar function in glibc-2.2.
 *      parse_redirect, redirect_opt_num, and big chunks of main
 *      and many builtins derived from contributions by Erik Andersen.
 *      Miscellaneous bugfixes from Matt Kraai.
 *
 * There are two big (and related) architecture differences between
 * this parser and the lash parser.  One is that this version is
 * actually designed from the ground up to understand nearly all
 * of the Bourne grammar.  The second, consequential change is that
 * the parser and input reader have been turned inside out.  Now,
 * the parser is in control, and asks for input as needed.  The old
 * way had the input reader in control, and it asked for parsing to
 * take place as needed.  The new way makes it much easier to properly
 * handle the recursion implicit in the various substitutions, especially
 * across continuation lines.
 *
 * POSIX syntax not implemented:
 *      aliases
 *      <(list) and >(list) Process Substitution
 *      Functions
 *      Tilde Expansion
 *
 * Bash stuff (maybe optionally enable?):
 *      &> and >& redirection of stdout+stderr
 *      Brace expansion
 *      reserved words: [[ ]] function select
 *      substrings ${var:1:5}
 *
 * TODOs:
 *      grep for "TODO" and fix (some of them are easy)
 *      change { and } from special chars to reserved words
 *      builtins: return, ulimit
 *      follow IFS rules more precisely, including update semantics
 *      figure out what to do with backslash-newline
 *      continuation lines, both explicit and implicit - done?
 *      SIGHUP handling
 *      ^Z handling (and explain it in comments for mere humans)
 *      separate job control from interactiveness
 *      (testcase: booting with init=/bin/hush does not show prompt (2009-04))
 *      functions
 *
 * Licensed under the GPL v2 or later, see the file LICENSE in this tarball.
 */

#include "busybox.h" /* for APPLET_IS_NOFORK/NOEXEC */
//TODO: pull in some .h and find out whether we have SINGLE_APPLET_MAIN?
//#include "applet_tables.h" doesn't work
#include <glob.h>
/* #include <dmalloc.h> */
#if ENABLE_HUSH_CASE
#include <fnmatch.h>
#endif
#include "math.h"
#include "match.h"
#ifndef PIPE_BUF
# define PIPE_BUF 4096           /* amount of buffering in a pipe */
#endif

#ifdef WANT_TO_TEST_NOMMU
# undef BB_MMU
# undef USE_FOR_NOMMU
# undef USE_FOR_MMU
# define BB_MMU 0
# define USE_FOR_NOMMU(...) __VA_ARGS__
# define USE_FOR_MMU(...)
#endif

#if defined SINGLE_APPLET_MAIN
/* STANDALONE does not make sense, and won't compile */
#undef CONFIG_FEATURE_SH_STANDALONE
#undef ENABLE_FEATURE_SH_STANDALONE
#undef USE_FEATURE_SH_STANDALONE
#define SKIP_FEATURE_SH_STANDALONE(...) __VA_ARGS__
#define ENABLE_FEATURE_SH_STANDALONE 0
#define USE_FEATURE_SH_STANDALONE(...)
#define SKIP_FEATURE_SH_STANDALONE(...) __VA_ARGS__
#endif

#if !ENABLE_HUSH_INTERACTIVE
#undef ENABLE_FEATURE_EDITING
#define ENABLE_FEATURE_EDITING 0
#undef ENABLE_FEATURE_EDITING_FANCY_PROMPT
#define ENABLE_FEATURE_EDITING_FANCY_PROMPT 0
#endif

/* Do we support ANY keywords? */
#if ENABLE_HUSH_IF || ENABLE_HUSH_LOOPS || ENABLE_HUSH_CASE
#define HAS_KEYWORDS 1
#define IF_HAS_KEYWORDS(...) __VA_ARGS__
#define IF_HAS_NO_KEYWORDS(...)
#else
#define HAS_KEYWORDS 0
#define IF_HAS_KEYWORDS(...)
#define IF_HAS_NO_KEYWORDS(...) __VA_ARGS__
#endif

/* Enable/disable sanity checks. Ok to enable in production,
 * only adds a bit of bloat.
 * Keeping unconditionally on for now.
 */
#define HUSH_DEBUG 1
/* In progress... */
#define ENABLE_HUSH_FUNCTIONS 0


/* If you comment out one of these below, it will be #defined later
 * to perform debug printfs to stderr: */
#define debug_printf(...)        do {} while (0)
/* Finer-grained debug switches */
#define debug_printf_parse(...)  do {} while (0)
#define debug_print_tree(a, b)   do {} while (0)
#define debug_printf_exec(...)   do {} while (0)
#define debug_printf_env(...)    do {} while (0)
#define debug_printf_jobs(...)   do {} while (0)
#define debug_printf_expand(...) do {} while (0)
#define debug_printf_glob(...)   do {} while (0)
#define debug_printf_list(...)   do {} while (0)
#define debug_printf_subst(...)  do {} while (0)
#define debug_printf_clean(...)  do {} while (0)

#ifndef debug_printf
#define debug_printf(...) fprintf(stderr, __VA_ARGS__)
#endif

#ifndef debug_printf_parse
#define debug_printf_parse(...) fprintf(stderr, __VA_ARGS__)
#endif

#ifndef debug_printf_exec
#define debug_printf_exec(...) fprintf(stderr, __VA_ARGS__)
#endif

#ifndef debug_printf_env
#define debug_printf_env(...) fprintf(stderr, __VA_ARGS__)
#endif

#ifndef debug_printf_jobs
#define debug_printf_jobs(...) fprintf(stderr, __VA_ARGS__)
#define DEBUG_JOBS 1
#else
#define DEBUG_JOBS 0
#endif

#ifndef debug_printf_expand
#define debug_printf_expand(...) fprintf(stderr, __VA_ARGS__)
#define DEBUG_EXPAND 1
#else
#define DEBUG_EXPAND 0
#endif

#ifndef debug_printf_glob
#define debug_printf_glob(...) fprintf(stderr, __VA_ARGS__)
#define DEBUG_GLOB 1
#else
#define DEBUG_GLOB 0
#endif

#ifndef debug_printf_list
#define debug_printf_list(...) fprintf(stderr, __VA_ARGS__)
#endif

#ifndef debug_printf_subst
#define debug_printf_subst(...) fprintf(stderr, __VA_ARGS__)
#endif

#ifndef debug_printf_clean
/* broken, of course, but OK for testing */
static const char *indenter(int i)
{
	static const char blanks[] ALIGN1 =
		"                                    ";
	return &blanks[sizeof(blanks) - i - 1];
}
#define debug_printf_clean(...) fprintf(stderr, __VA_ARGS__)
#define DEBUG_CLEAN 1
#else
#define DEBUG_CLEAN 0
#endif

#if DEBUG_EXPAND
static void debug_print_strings(const char *prefix, char **vv)
{
	fprintf(stderr, "%s:\n", prefix);
	while (*vv)
		fprintf(stderr, " '%s'\n", *vv++);
}
#else
#define debug_print_strings(prefix, vv) ((void)0)
#endif

/*
 * Leak hunting. Use hush_leaktool.sh for post-processing.
 */
//#define FOR_HUSH_LEAKTOOL
#ifdef FOR_HUSH_LEAKTOOL
static void *xxmalloc(int lineno, size_t size)
{
	void *ptr = xmalloc((size + 0xff) & ~0xff);
	fdprintf(2, "line %d: malloc %p\n", lineno, ptr);
	return ptr;
}
static void *xxrealloc(int lineno, void *ptr, size_t size)
{
	ptr = xrealloc(ptr, (size + 0xff) & ~0xff);
	fdprintf(2, "line %d: realloc %p\n", lineno, ptr);
	return ptr;
}
static char *xxstrdup(int lineno, const char *str)
{
	char *ptr = xstrdup(str);
	fdprintf(2, "line %d: strdup %p\n", lineno, ptr);
	return ptr;
}
static void xxfree(void *ptr)
{
	fdprintf(2, "free %p\n", ptr);
	free(ptr);
}
#define xmalloc(s)     xxmalloc(__LINE__, s)
#define xrealloc(p, s) xxrealloc(__LINE__, p, s)
#define xstrdup(s)     xxstrdup(__LINE__, s)
#define free(p)        xxfree(p)
#endif


#define ERR_PTR ((void*)(long)1)

#define JOB_STATUS_FORMAT "[%d] %-22s %.40s\n"

#define SPECIAL_VAR_SYMBOL 3

static const char hush_version_str[] ALIGN1 = "HUSH_VERSION="BB_VER;

/* This supports saving pointers malloced in vfork child,
 * to be freed in the parent. One pointer is saved in
 * G.argv_from_re_execing global var instead. TODO: unify.
 */
#if !BB_MMU
typedef struct nommu_save_t {
	char **new_env;
	char **old_env;
	char **argv;
} nommu_save_t;
#endif

/* The descrip member of this structure is only used to make
 * debugging output pretty */
static const struct {
	int mode;
	signed char default_fd;
	char descrip[3];
} redir_table[] = {
	{ 0,                         0, "??" },
	{ O_RDONLY,                  0, "<"  },
	{ O_CREAT|O_TRUNC|O_WRONLY,  1, ">"  },
	{ O_CREAT|O_APPEND|O_WRONLY, 1, ">>" },
	{ O_RDONLY,                  0, "<<" },
	{ O_CREAT|O_RDWR,            1, "<>" },
/* Should not be needed. Bogus default_fd helps in debugging */
/*	{ O_RDONLY,                 77, "<<" }, */
};

typedef enum reserved_style {
	RES_NONE  = 0,
#if ENABLE_HUSH_IF
	RES_IF    ,
	RES_THEN  ,
	RES_ELIF  ,
	RES_ELSE  ,
	RES_FI    ,
#endif
#if ENABLE_HUSH_LOOPS
	RES_FOR   ,
	RES_WHILE ,
	RES_UNTIL ,
	RES_DO    ,
	RES_DONE  ,
#endif
#if ENABLE_HUSH_LOOPS || ENABLE_HUSH_CASE
	RES_IN    ,
#endif
#if ENABLE_HUSH_CASE
	RES_CASE  ,
	/* two pseudo-keywords support contrived "case" syntax: */
	RES_MATCH , /* "word)" */
	RES_CASEI , /* "this command is inside CASE" */
	RES_ESAC  ,
#endif
	RES_XXXX  ,
	RES_SNTX
} reserved_style;

typedef struct o_string {
	char *data;
	int length; /* position where data is appended */
	int maxlen;
	/* Protect newly added chars against globbing
	 * (by prepending \ to *, ?, [, \) */
	smallint o_escape;
	smallint o_glob;
	/* At least some part of the string was inside '' or "",
	 * possibly empty one: word"", wo''rd etc. */
	smallint o_quoted;
	smallint has_empty_slot;
	smallint o_assignment; /* 0:maybe, 1:yes, 2:no */
} o_string;
enum {
	MAYBE_ASSIGNMENT = 0,
	DEFINITELY_ASSIGNMENT = 1,
	NOT_ASSIGNMENT = 2,
	WORD_IS_KEYWORD = 3, /* not assigment, but next word may be: "if v=xyz cmd;" */
};
/* Used for initialization: o_string foo = NULL_O_STRING; */
#define NULL_O_STRING { NULL }

/* I can almost use ordinary FILE*.  Is open_memstream() universally
 * available?  Where is it documented? */
typedef struct in_str {
	const char *p;
	/* eof_flag=1: last char in ->p is really an EOF */
	char eof_flag; /* meaningless if ->p == NULL */
	char peek_buf[2];
#if ENABLE_HUSH_INTERACTIVE
	smallint promptme;
	smallint promptmode; /* 0: PS1, 1: PS2 */
#endif
	FILE *file;
	int (*get) (struct in_str *);
	int (*peek) (struct in_str *);
} in_str;
#define i_getch(input) ((input)->get(input))
#define i_peek(input) ((input)->peek(input))

struct redir_struct {
	struct redir_struct *next;
	char *rd_filename;          /* filename */
	int rd_fd;                  /* fd to redirect */
	/* fd to redirect to, or -3 if rd_fd is to be closed (n>&-) */
	int rd_dup;
	smallint rd_type;           /* (enum redir_type) */
	/* note: for heredocs, rd_filename contains heredoc delimiter,
	 * and subsequently heredoc itself; and rd_dup is a bitmask:
	 * 1: do we need to trim leading tabs?
	 * 2: is heredoc quoted (<<'dleim' syntax) ?
	 */
};
typedef enum redir_type {
	REDIRECT_INVALID   = 0,
	REDIRECT_INPUT     = 1,
	REDIRECT_OVERWRITE = 2,
	REDIRECT_APPEND    = 3,
	REDIRECT_HEREDOC   = 4,
	REDIRECT_IO        = 5,
	REDIRECT_HEREDOC2  = 6, /* REDIRECT_HEREDOC after heredoc is loaded */
	REDIRFD_CLOSE = -3,
	HEREDOC_SKIPTABS = 1,
	HEREDOC_QUOTED   = 2,
} redir_type;


struct command {
	pid_t pid;                  /* 0 if exited */
	int assignment_cnt;         /* how many argv[i] are assignments? */
	smallint is_stopped;        /* is the command currently running? */
	smallint grp_type;          /* GRP_xxx */
	struct pipe *group;         /* if non-NULL, this "command" is { list },
	                             * ( list ), or a compound statement */
#if !BB_MMU
	char *group_as_string;
#endif
	char **argv;                /* command name and arguments */
	struct redir_struct *redirects; /* I/O redirections */
};
/* argv vector may contain variable references (^Cvar^C, ^C0^C etc)
 * and on execution these are substituted with their values.
 * Substitution can make _several_ words out of one argv[n]!
 * Example: argv[0]=='.^C*^C.' here: echo .$*.
 * References of the form ^C`cmd arg^C are `cmd arg` substitutions.
 */
#define GRP_NORMAL   0
#define GRP_SUBSHELL 1
#if ENABLE_HUSH_FUNCTIONS
#define GRP_FUNCTION 2
#endif

struct pipe {
	struct pipe *next;
	int num_cmds;               /* total number of commands in job */
	int alive_cmds;             /* number of commands running (not exited) */
	int stopped_cmds;           /* number of commands alive, but stopped */
#if ENABLE_HUSH_JOB
	int jobid;                  /* job number */
	pid_t pgrp;                 /* process group ID for the job */
	char *cmdtext;              /* name of job */
#endif
	struct command *cmds;       /* array of commands in pipe */
	smallint followup;          /* PIPE_BG, PIPE_SEQ, PIPE_OR, PIPE_AND */
	IF_HAS_KEYWORDS(smallint pi_inverted;) /* "! cmd | cmd" */
	IF_HAS_KEYWORDS(smallint res_word;) /* needed for if, for, while, until... */
};
typedef enum pipe_style {
	PIPE_SEQ = 1,
	PIPE_AND = 2,
	PIPE_OR  = 3,
	PIPE_BG  = 4,
} pipe_style;

/* This holds pointers to the various results of parsing */
struct parse_context {
	/* linked list of pipes */
	struct pipe *list_head;
	/* last pipe (being constructed right now) */
	struct pipe *pipe;
	/* last command in pipe (being constructed right now) */
	struct command *command;
	/* last redirect in command->redirects list */
	struct redir_struct *pending_redirect;
#if !BB_MMU
	o_string as_string;
#endif
#if HAS_KEYWORDS
	smallint ctx_res_w;
	smallint ctx_inverted; /* "! cmd | cmd" */
#if ENABLE_HUSH_CASE
	smallint ctx_dsemicolon; /* ";;" seen */
#endif
	/* bitmask of FLAG_xxx, for figuring out valid reserved words */
	int old_flag;
	/* group we are enclosed in:
	 * example: "if pipe1; pipe2; then pipe3; fi"
	 * when we see "if" or "then", we malloc and copy current context,
	 * and make ->stack point to it. then we parse pipeN.
	 * when closing "then" / fi" / whatever is found,
	 * we move list_head into ->stack->command->group,
	 * copy ->stack into current context, and delete ->stack.
	 * (parsing of { list } and ( list ) doesn't use this method)
	 */
	struct parse_context *stack;
#endif
};

/* On program start, environ points to initial environment.
 * putenv adds new pointers into it, unsetenv removes them.
 * Neither of these (de)allocates the strings.
 * setenv allocates new strings in malloc space and does putenv,
 * and thus setenv is unusable (leaky) for shell's purposes */
#define setenv(...) setenv_is_leaky_dont_use()
struct variable {
	struct variable *next;
	char *varstr;        /* points to "name=" portion */
	int max_len;         /* if > 0, name is part of initial env; else name is malloced */
	smallint flg_export; /* putenv should be done on this var */
	smallint flg_read_only;
};

enum {
	BC_BREAK = 1,
	BC_CONTINUE = 2,
};


/* "Globals" within this file */
/* Sorted roughly by size (smaller offsets == smaller code) */
struct globals {
#if ENABLE_HUSH_INTERACTIVE
	/* 'interactive_fd' is a fd# open to ctty, if we have one
	 * _AND_ if we decided to act interactively */
	int interactive_fd;
	const char *PS1;
	const char *PS2;
#define G_interactive_fd (G.interactive_fd)
#else
#define G_interactive_fd 0
#endif
#if ENABLE_FEATURE_EDITING
	line_input_t *line_input_state;
#endif
	pid_t root_pid;
	pid_t last_bg_pid;
#if ENABLE_HUSH_JOB
	int run_list_level;
	pid_t saved_tty_pgrp;
	int last_jobid;
	struct pipe *job_list;
	struct pipe *toplevel_list;
////	smallint ctrl_z_flag;
#endif
	smallint flag_SIGINT;
#if ENABLE_HUSH_LOOPS
	smallint flag_break_continue;
#endif
	smallint fake_mode;
	/* These four support $?, $#, and $1 */
	smalluint last_exitcode;
	/* are global_argv and global_argv[1..n] malloced? (note: not [0]) */
	smalluint global_args_malloced;
	/* how many non-NULL argv's we have. NB: $# + 1 */
	int global_argc;
	char **global_argv;
#if !BB_MMU
	char *argv0_for_re_execing;
	char **argv_from_re_execing;
#endif
#if ENABLE_HUSH_LOOPS
	unsigned depth_break_continue;
	unsigned depth_of_loop;
#endif
	const char *ifs;
	const char *cwd;
	struct variable *top_var; /* = &G.shell_ver (set in main()) */
	struct variable shell_ver;
	/* Signal and trap handling */
//	unsigned count_SIGCHLD;
//	unsigned handled_SIGCHLD;
	/* which signals have non-DFL handler (even with no traps set)? */
	unsigned non_DFL_mask;
	char **traps; /* char *traps[NSIG] */
	sigset_t blocked_set;
	sigset_t inherited_set;
#if HUSH_DEBUG
	unsigned long memleak_value;
#endif
	char user_input_buf[ENABLE_FEATURE_EDITING ? BUFSIZ : 2];
#if ENABLE_FEATURE_SH_STANDALONE
	struct nofork_save_area nofork_save;
#endif
#if ENABLE_HUSH_JOB
	sigjmp_buf toplevel_jb;
#endif
};
#define G (*ptr_to_globals)
/* Not #defining name to G.name - this quickly gets unwieldy
 * (too many defines). Also, I actually prefer to see when a variable
 * is global, thus "G." prefix is a useful hint */
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
} while (0)


/* Function prototypes for builtins */
static int builtin_cd(char **argv);
static int builtin_echo(char **argv);
static int builtin_eval(char **argv);
static int builtin_exec(char **argv);
static int builtin_exit(char **argv);
static int builtin_export(char **argv);
#if ENABLE_HUSH_JOB
static int builtin_fg_bg(char **argv);
static int builtin_jobs(char **argv);
#endif
#if ENABLE_HUSH_HELP
static int builtin_help(char **argv);
#endif
#if HUSH_DEBUG
static int builtin_memleak(char **argv);
#endif
static int builtin_pwd(char **argv);
static int builtin_read(char **argv);
static int builtin_set(char **argv);
static int builtin_shift(char **argv);
static int builtin_source(char **argv);
static int builtin_test(char **argv);
static int builtin_trap(char **argv);
static int builtin_true(char **argv);
static int builtin_umask(char **argv);
static int builtin_unset(char **argv);
static int builtin_wait(char **argv);
#if ENABLE_HUSH_LOOPS
static int builtin_break(char **argv);
static int builtin_continue(char **argv);
#endif

/* Table of built-in functions.  They can be forked or not, depending on
 * context: within pipes, they fork.  As simple commands, they do not.
 * When used in non-forking context, they can change global variables
 * in the parent shell process.  If forked, of course they cannot.
 * For example, 'unset foo | whatever' will parse and run, but foo will
 * still be set at the end. */
struct built_in_command {
	const char *cmd;
	int (*function)(char **argv);
#if ENABLE_HUSH_HELP
	const char *descr;
#define BLTIN(cmd, func, help) { cmd, func, help }
#else
#define BLTIN(cmd, func, help) { cmd, func }
#endif
};

/* For now, echo and test are unconditionally enabled.
 * Maybe make it configurable? */
static const struct built_in_command bltins[] = {
	BLTIN("."       , builtin_source  , "Run commands in a file"),
	BLTIN(":"       , builtin_true    , "No-op"),
	BLTIN("["       , builtin_test    , "Test condition"),
#if ENABLE_HUSH_JOB
	BLTIN("bg"      , builtin_fg_bg   , "Resume a job in the background"),
#endif
#if ENABLE_HUSH_LOOPS
	BLTIN("break"   , builtin_break   , "Exit from a loop"),
#endif
	BLTIN("cd"      , builtin_cd      , "Change directory"),
#if ENABLE_HUSH_LOOPS
	BLTIN("continue", builtin_continue, "Start new loop iteration"),
#endif
	BLTIN("echo"    , builtin_echo    , "Write to stdout"),
	BLTIN("eval"    , builtin_eval    , "Construct and run shell command"),
	BLTIN("exec"    , builtin_exec    , "Execute command, don't return to shell"),
	BLTIN("exit"    , builtin_exit    , "Exit"),
	BLTIN("export"  , builtin_export  , "Set environment variable"),
#if ENABLE_HUSH_JOB
	BLTIN("fg"      , builtin_fg_bg   , "Bring job into the foreground"),
#endif
#if ENABLE_HUSH_HELP
	BLTIN("help"    , builtin_help    , "List shell built-in commands"),
#endif
#if ENABLE_HUSH_JOB
	BLTIN("jobs"    , builtin_jobs    , "List active jobs"),
#endif
#if HUSH_DEBUG
	BLTIN("memleak" , builtin_memleak , "Debug tool"),
#endif
	BLTIN("pwd"     , builtin_pwd     , "Print current directory"),
	BLTIN("read"    , builtin_read    , "Input environment variable"),
//	BLTIN("return"  , builtin_return  , "Return from a function"),
	BLTIN("set"     , builtin_set     , "Set/unset shell local variables"),
	BLTIN("shift"   , builtin_shift   , "Shift positional parameters"),
	BLTIN("test"    , builtin_test    , "Test condition"),
	BLTIN("trap"    , builtin_trap    , "Trap signals"),
//	BLTIN("ulimit"  , builtin_return  , "Control resource limits"),
	BLTIN("umask"   , builtin_umask   , "Set file creation mask"),
	BLTIN("unset"   , builtin_unset   , "Unset environment variable"),
	BLTIN("wait"    , builtin_wait    , "Wait for process"),
};


static void maybe_die(const char *notice, const char *msg)
{
	/* Was using fancy stuff:
	 * (G_interactive_fd ? bb_error_msg : bb_error_msg_and_die)(...params...)
	 * but it SEGVs. ?! Oh well... explicit temp ptr works around that */
	void FAST_FUNC (*fp)(const char *s, ...) = bb_error_msg_and_die;
#if ENABLE_HUSH_INTERACTIVE
	if (G_interactive_fd)
		fp = bb_error_msg;
#endif
	fp(msg ? "%s: %s" : notice, notice, msg);
}
#if 1
#define syntax(msg) maybe_die("syntax error", msg);
#else
/* Debug -- trick gcc to expand __LINE__ and convert to string */
#define __syntax(msg, line) maybe_die("syntax error hush.c:" # line, msg)
#define _syntax(msg, line) __syntax(msg, line)
#define syntax(msg) _syntax(msg, __LINE__)
#endif


static int glob_needed(const char *s)
{
	while (*s) {
		if (*s == '\\')
			s++;
		if (*s == '*' || *s == '[' || *s == '?')
			return 1;
		s++;
	}
	return 0;
}

static int is_assignment(const char *s)
{
	if (!s || !(isalpha(*s) || *s == '_'))
		return 0;
	s++;
	while (isalnum(*s) || *s == '_')
		s++;
	return *s == '=';
}

/* Replace each \x with x in place, return ptr past NUL. */
static char *unbackslash(char *src)
{
	char *dst = src;
	while (1) {
		if (*src == '\\')
			src++;
		if ((*dst++ = *src++) == '\0')
			break;
	}
	return dst;
}

static char **add_strings_to_strings(char **strings, char **add, int need_to_dup)
{
	int i;
	unsigned count1;
	unsigned count2;
	char **v;

	v = strings;
	count1 = 0;
	if (v) {
		while (*v) {
			count1++;
			v++;
		}
	}
	count2 = 0;
	v = add;
	while (*v) {
		count2++;
		v++;
	}
	v = xrealloc(strings, (count1 + count2 + 1) * sizeof(char*));
	v[count1 + count2] = NULL;
	i = count2;
	while (--i >= 0)
		v[count1 + i] = (need_to_dup ? xstrdup(add[i]) : add[i]);
	return v;
}
#ifdef FOR_HUSH_LEAKTOOL
static char **xx_add_strings_to_strings(int lineno, char **strings, char **add, int need_to_dup)
{
	char **ptr = add_strings_to_strings(strings, add, need_to_dup);
	fdprintf(2, "line %d: add_strings_to_strings %p\n", lineno, ptr);
	return ptr;
}
#define add_strings_to_strings(strings, add, need_to_dup) \
	xx_add_strings_to_strings(__LINE__, strings, add, need_to_dup)
#endif

static char **add_string_to_strings(char **strings, char *add)
{
	char *v[2];
	v[0] = add;
	v[1] = NULL;
	return add_strings_to_strings(strings, v, /*dup:*/ 0);
}
#ifdef FOR_HUSH_LEAKTOOL
static char **xx_add_string_to_strings(int lineno, char **strings, char *add)
{
	char **ptr = add_string_to_strings(strings, add);
	fdprintf(2, "line %d: add_string_to_strings %p\n", lineno, ptr);
	return ptr;
}
#define add_string_to_strings(strings, add) \
	xx_add_string_to_strings(__LINE__, strings, add)
#endif

static void putenv_all(char **strings)
{
	if (!strings)
		return;
	while (*strings) {
		debug_printf_env("putenv '%s'\n", *strings);
		putenv(*strings++);
	}
}

static char **putenv_all_and_save_old(char **strings)
{
	char **old = NULL;
	char **s = strings;

	if (!strings)
		return old;
	while (*strings) {
		char *v, *eq;

		eq = strchr(*strings, '=');
		if (eq) {
			*eq = '\0';
			v = getenv(*strings);
			*eq = '=';
			if (v) {
				/* v points to VAL in VAR=VAL, go back to VAR */
				v -= (eq - *strings) + 1;
				old = add_string_to_strings(old, v);
			}
		}
		strings++;
	}
	putenv_all(s);
	return old;
}

static void free_strings_and_unsetenv(char **strings, int unset)
{
	char **v;

	if (!strings)
		return;

	v = strings;
	while (*v) {
		if (unset) {
			debug_printf_env("unsetenv '%s'\n", *v);
			bb_unsetenv(*v);
		}
		free(*v++);
	}
	free(strings);
}

static void free_strings(char **strings)
{
	free_strings_and_unsetenv(strings, 0);
}


/* Basic theory of signal handling in shell
 * ========================================
 * This does not describe what hush does, rather, it is current understanding
 * what it _should_ do. If it doesn't, it's a bug.
 * http://www.opengroup.org/onlinepubs/9699919799/utilities/V3_chap02.html#trap
 *
 * Signals are handled only after each pipe ("cmd | cmd | cmd" thing)
 * is finished or backgrounded. It is the same in interactive and
 * non-interactive shells, and is the same regardless of whether
 * a user trap handler is installed or a shell special one is in effect.
 * ^C or ^Z from keyboard seem to execute "at once" because it usually
 * backgrounds (i.e. stops) or kills all members of currently running
 * pipe.
 *
 * Wait builtin in interruptible by signals for which user trap is set
 * or by SIGINT in interactive shell.
 *
 * Trap handlers will execute even within trap handlers. (right?)
 *
 * User trap handlers are forgotten when subshell ("(cmd)") is entered. [TODO]
 *
 * If job control is off, backgrounded commands ("cmd &")
 * have SIGINT, SIGQUIT set to SIG_IGN.
 *
 * Commands run in command substitution ("`cmd`")
 * have SIGTTIN, SIGTTOU, SIGTSTP set to SIG_IGN.
 *
 * Ordinary commands have signals set to SIG_IGN/DFL set as inherited
 * by the shell from its parent.
 *
 * Siganls which differ from SIG_DFL action
 * (note: child (i.e., [v]forked) shell is not an interactive shell):
 *
 * SIGQUIT: ignore
 * SIGTERM (interactive): ignore
 * SIGHUP (interactive):
 *    send SIGCONT to stopped jobs, send SIGHUP to all jobs and exit
 * SIGTTIN, SIGTTOU, SIGTSTP (if job control is on): ignore
 *    (note that ^Z is handled not by trapping SIGTSTP, but by seeing
 *    that all pipe members are stopped) (right?)
 * SIGINT (interactive): wait for last pipe, ignore the rest
 *    of the command line, show prompt. NB: ^C does not send SIGINT
 *    to interactive shell while shell is waiting for a pipe,
 *    since shell is bg'ed (is not in foreground process group).
 *    (check/expand this)
 *    Example 1: this waits 5 sec, but does not execute ls:
 *    "echo $$; sleep 5; ls -l" + "kill -INT <pid>"
 *    Example 2: this does not wait and does not execute ls:
 *    "echo $$; sleep 5 & wait; ls -l" + "kill -INT <pid>"
 *    Example 3: this does not wait 5 sec, but executes ls:
 *    "sleep 5; ls -l" + press ^C
 *
 * (What happens to signals which are IGN on shell start?)
 * (What happens with signal mask on shell start?)
 *
 * Implementation in hush
 * ======================
 * We use in-kernel pending signal mask to determine which signals were sent.
 * We block all signals which we don't want to take action immediately,
 * i.e. we block all signals which need to have special handling as described
 * above, and all signals which have traps set.
 * After each pipe execution, we extract any pending signals via sigtimedwait()
 * and act on them.
 *
 * unsigned non_DFL_mask: a mask of such "special" signals
 * sigset_t blocked_set:  current blocked signal set
 *
 * "trap - SIGxxx":
 *    clear bit in blocked_set unless it is also in non_DFL_mask
 * "trap 'cmd' SIGxxx":
 *    set bit in blocked_set (even if 'cmd' is '')
 * after [v]fork, if we plan to be a shell:
 *    nothing for {} child shell (say, "true | { true; true; } | true")
 *    unset all traps if () shell. [TODO]
 * after [v]fork, if we plan to exec:
 *    POSIX says pending signal mask is cleared in child - no need to clear it.
 *    Restore blocked signal set to one inherited by shell just prior to exec.
 *
 * Note: as a result, we do not use signal handlers much. The only uses
 * are to count SIGCHLDs [disabled - bug somewhere, + bloat]
 * and to restore tty pgrp on signal-induced exit.
 */

//static void SIGCHLD_handler(int sig UNUSED_PARAM)
//{
//	G.count_SIGCHLD++;
//}

static int check_and_run_traps(int sig)
{
	static const struct timespec zero_timespec = { 0, 0 };
	smalluint save_rcode;
	int last_sig = 0;

	if (sig)
		goto jump_in;
	while (1) {
		sig = sigtimedwait(&G.blocked_set, NULL, &zero_timespec);
		if (sig <= 0)
			break;
 jump_in:
		last_sig = sig;
		if (G.traps && G.traps[sig]) {
			if (G.traps[sig][0]) {
				/* We have user-defined handler */
				char *argv[] = { NULL, xstrdup(G.traps[sig]), NULL };
				save_rcode = G.last_exitcode;
				builtin_eval(argv);
				free(argv[1]);
				G.last_exitcode = save_rcode;
			} /* else: "" trap, ignoring signal */
			continue;
		}
		/* not a trap: special action */
		switch (sig) {
//		case SIGCHLD:
//			G.count_SIGCHLD++;
//			break;
		case SIGINT:
			bb_putchar('\n');
			G.flag_SIGINT = 1;
			break;
//TODO
//		case SIGHUP: ...
//			break;
		default: /* ignored: */
			/* SIGTERM, SIGQUIT, SIGTTIN, SIGTTOU, SIGTSTP */
			break;
		}
	}
	return last_sig;
}

#if ENABLE_HUSH_JOB

/* After [v]fork, in child: do not restore tty pgrp on xfunc death */
#define disable_restore_tty_pgrp_on_exit() (die_sleep = 0)
/* After [v]fork, in parent: restore tty pgrp on xfunc death */
#define enable_restore_tty_pgrp_on_exit()  (die_sleep = -1)

/* Restores tty foreground process group, and exits.
 * May be called as signal handler for fatal signal
 * (will faithfully resend signal to itself, producing correct exit state)
 * or called directly with -EXITCODE.
 * We also call it if xfunc is exiting. */
static void sigexit(int sig) NORETURN;
static void sigexit(int sig)
{
	/* Disable all signals: job control, SIGPIPE, etc. */
	sigprocmask_allsigs(SIG_BLOCK);

	/* Careful: we can end up here after [v]fork. Do not restore
	 * tty pgrp then, only top-level shell process does that */
	if (G_interactive_fd && getpid() == G.root_pid)
		tcsetpgrp(G_interactive_fd, G.saved_tty_pgrp);

	/* Not a signal, just exit */
	if (sig <= 0)
		_exit(- sig);

	kill_myself_with_sig(sig); /* does not return */
}
#else

#define disable_restore_tty_pgrp_on_exit() ((void)0)
#define enable_restore_tty_pgrp_on_exit()  ((void)0)

#endif

/* Restores tty foreground process group, and exits. */
static void hush_exit(int exitcode) NORETURN;
static void hush_exit(int exitcode)
{
	if (G.traps && G.traps[0] && G.traps[0][0]) {
		char *argv[] = { NULL, xstrdup(G.traps[0]), NULL };
//TODO: do we need to prevent recursion?
		builtin_eval(argv);
		free(argv[1]);
	}

#if ENABLE_HUSH_JOB
	fflush(NULL); /* flush all streams */
	sigexit(- (exitcode & 0xff));
#else
	exit(exitcode);
#endif
}


static const char *set_cwd(void)
{
	/* xrealloc_getcwd_or_warn(arg) calls free(arg),
	 * we must not try to free(bb_msg_unknown) */
	if (G.cwd == bb_msg_unknown)
		G.cwd = NULL;
	G.cwd = xrealloc_getcwd_or_warn((char *)G.cwd);
	if (!G.cwd)
		G.cwd = bb_msg_unknown;
	return G.cwd;
}


/* Get/check local shell variables */
static struct variable *get_local_var(const char *name)
{
	struct variable *cur;
	int len;

	if (!name)
		return NULL;
	len = strlen(name);
	for (cur = G.top_var; cur; cur = cur->next) {
		if (strncmp(cur->varstr, name, len) == 0 && cur->varstr[len] == '=')
			return cur;
	}
	return NULL;
}

static const char *get_local_var_value(const char *src)
{
	struct variable *var = get_local_var(src);
	if (var)
		return strchr(var->varstr, '=') + 1;
	return NULL;
}

/* str holds "NAME=VAL" and is expected to be malloced.
 * We take ownership of it.
 * flg_export:
 *  0: do not export
 *  1: export
 * -1: if NAME is set, leave export status alone
 *     if NAME is not set, do not export
 * flg_read_only is set only when we handle -R var=val
 */
#if BB_MMU
#define set_local_var(str, flg_export, flg_read_only) \
	set_local_var(str, flg_export)
#endif
static int set_local_var(char *str, int flg_export, int flg_read_only)
{
	struct variable *cur;
	char *value;
	int name_len;

	value = strchr(str, '=');
	if (!value) { /* not expected to ever happen? */
		free(str);
		return -1;
	}

	name_len = value - str + 1; /* including '=' */
	cur = G.top_var; /* cannot be NULL (we have HUSH_VERSION and it's RO) */
	while (1) {
		if (strncmp(cur->varstr, str, name_len) != 0) {
			if (!cur->next) {
				/* Bail out. Note that now cur points
				 * to last var in linked list */
				break;
			}
			cur = cur->next;
			continue;
		}
		/* We found an existing var with this name */
		*value = '\0';
		if (cur->flg_read_only) {
#if !BB_MMU
			if (!flg_read_only)
#endif
				bb_error_msg("%s: readonly variable", str);
			free(str);
			return -1;
		}
		debug_printf_env("%s: unsetenv '%s'\n", __func__, str);
		unsetenv(str); /* just in case */
		*value = '=';
		if (strcmp(cur->varstr, str) == 0) {
 free_and_exp:
			free(str);
			goto exp;
		}
		if (cur->max_len >= strlen(str)) {
			/* This one is from startup env, reuse space */
			strcpy(cur->varstr, str);
			goto free_and_exp;
		}
		/* max_len == 0 signifies "malloced" var, which we can
		 * (and has to) free */
		if (!cur->max_len)
			free(cur->varstr);
		cur->max_len = 0;
		goto set_str_and_exp;
	}

	/* Not found - create next variable struct */
	cur->next = xzalloc(sizeof(*cur));
	cur = cur->next;

 set_str_and_exp:
	cur->varstr = str;
#if !BB_MMU
	cur->flg_read_only = flg_read_only;
#endif
 exp:
	if (flg_export == 1)
		cur->flg_export = 1;
	if (cur->flg_export) {
		debug_printf_env("%s: putenv '%s'\n", __func__, cur->varstr);
		return putenv(cur->varstr);
	}
	return 0;
}

static int unset_local_var(const char *name)
{
	struct variable *cur;
	struct variable *prev = prev; /* for gcc */
	int name_len;

	if (!name)
		return EXIT_SUCCESS;
	name_len = strlen(name);
	cur = G.top_var;
	while (cur) {
		if (strncmp(cur->varstr, name, name_len) == 0 && cur->varstr[name_len] == '=') {
			if (cur->flg_read_only) {
				bb_error_msg("%s: readonly variable", name);
				return EXIT_FAILURE;
			}
			/* prev is ok to use here because 1st variable, HUSH_VERSION,
			 * is ro, and we cannot reach this code on the 1st pass */
			prev->next = cur->next;
			debug_printf_env("%s: unsetenv '%s'\n", __func__, cur->varstr);
			bb_unsetenv(cur->varstr);
			if (!cur->max_len)
				free(cur->varstr);
			free(cur);
			return EXIT_SUCCESS;
		}
		prev = cur;
		cur = cur->next;
	}
	return EXIT_SUCCESS;
}

#if ENABLE_SH_MATH_SUPPORT
#define is_name(c)      ((c) == '_' || isalpha((unsigned char)(c)))
#define is_in_name(c)   ((c) == '_' || isalnum((unsigned char)(c)))
static char *endofname(const char *name)
{
	char *p;

	p = (char *) name;
	if (!is_name(*p))
		return p;
	while (*++p) {
		if (!is_in_name(*p))
			break;
	}
	return p;
}

static void arith_set_local_var(const char *name, const char *val, int flags)
{
	/* arith code doesnt malloc space, so do it for it */
	char *var = xasprintf("%s=%s", name, val);
	set_local_var(var, flags, 0);
}
#endif


/*
 * in_str support
 */
static int static_get(struct in_str *i)
{
	int ch = *i->p++;
	if (ch != '\0')
		return ch;
	i->p--;
	return EOF;
}

static int static_peek(struct in_str *i)
{
	return *i->p;
}

#if ENABLE_HUSH_INTERACTIVE

static void cmdedit_set_initial_prompt(void)
{
	if (ENABLE_FEATURE_EDITING_FANCY_PROMPT) {
		G.PS1 = getenv("PS1");
		if (G.PS1 == NULL)
			G.PS1 = "\\w \\$ ";
	} else
		G.PS1 = NULL;
}

static const char* setup_prompt_string(int promptmode)
{
	const char *prompt_str;
	debug_printf("setup_prompt_string %d ", promptmode);
	if (!ENABLE_FEATURE_EDITING_FANCY_PROMPT) {
		/* Set up the prompt */
		if (promptmode == 0) { /* PS1 */
			free((char*)G.PS1);
			G.PS1 = xasprintf("%s %c ", G.cwd, (geteuid() != 0) ? '$' : '#');
			prompt_str = G.PS1;
		} else
			prompt_str = G.PS2;
	} else
		prompt_str = (promptmode == 0) ? G.PS1 : G.PS2;
	debug_printf("result '%s'\n", prompt_str);
	return prompt_str;
}

static void get_user_input(struct in_str *i)
{
	int r;
	const char *prompt_str;

	prompt_str = setup_prompt_string(i->promptmode);
#if ENABLE_FEATURE_EDITING
	/* Enable command line editing only while a command line
	 * is actually being read */
	do {
		G.flag_SIGINT = 0;
		/* buglet: SIGINT will not make new prompt to appear _at once_,
		 * only after <Enter>. (^C will work) */
		r = read_line_input(prompt_str, G.user_input_buf, BUFSIZ-1, G.line_input_state);
		/* catch *SIGINT* etc (^C is handled by read_line_input) */
		check_and_run_traps(0);
	} while (r == 0 || G.flag_SIGINT); /* repeat if ^C or SIGINT */
	i->eof_flag = (r < 0);
	if (i->eof_flag) { /* EOF/error detected */
		G.user_input_buf[0] = EOF; /* yes, it will be truncated, it's ok */
		G.user_input_buf[1] = '\0';
	}
#else
	do {
		G.flag_SIGINT = 0;
		fputs(prompt_str, stdout);
		fflush(stdout);
		G.user_input_buf[0] = r = fgetc(i->file);
		/*G.user_input_buf[1] = '\0'; - already is and never changed */
//do we need check_and_run_traps(0)? (maybe only if stdin)
	} while (G.flag_SIGINT);
	i->eof_flag = (r == EOF);
#endif
	i->p = G.user_input_buf;
}

#endif  /* INTERACTIVE */

/* This is the magic location that prints prompts
 * and gets data back from the user */
static int file_get(struct in_str *i)
{
	int ch;

	/* If there is data waiting, eat it up */
	if (i->p && *i->p) {
#if ENABLE_HUSH_INTERACTIVE
 take_cached:
#endif
		ch = *i->p++;
		if (i->eof_flag && !*i->p)
			ch = EOF;
		/* note: ch is never NUL */
	} else {
		/* need to double check i->file because we might be doing something
		 * more complicated by now, like sourcing or substituting. */
#if ENABLE_HUSH_INTERACTIVE
		if (G_interactive_fd && i->promptme && i->file == stdin) {
			do {
				get_user_input(i);
			} while (!*i->p); /* need non-empty line */
			i->promptmode = 1; /* PS2 */
			i->promptme = 0;
			goto take_cached;
		}
#endif
		do ch = fgetc(i->file); while (ch == '\0');
	}
	debug_printf("file_get: got '%c' %d\n", ch, ch);
#if ENABLE_HUSH_INTERACTIVE
	if (ch == '\n')
		i->promptme = 1;
#endif
	return ch;
}

/* All callers guarantee this routine will never
 * be used right after a newline, so prompting is not needed.
 */
static int file_peek(struct in_str *i)
{
	int ch;
	if (i->p && *i->p) {
		if (i->eof_flag && !i->p[1])
			return EOF;
		return *i->p;
		/* note: ch is never NUL */
	}
	do ch = fgetc(i->file); while (ch == '\0');
	i->eof_flag = (ch == EOF);
	i->peek_buf[0] = ch;
	i->peek_buf[1] = '\0';
	i->p = i->peek_buf;
	debug_printf("file_peek: got '%c' %d\n", ch, ch);
	return ch;
}

static void setup_file_in_str(struct in_str *i, FILE *f)
{
	i->peek = file_peek;
	i->get = file_get;
#if ENABLE_HUSH_INTERACTIVE
	i->promptme = 1;
	i->promptmode = 0; /* PS1 */
#endif
	i->file = f;
	i->p = NULL;
}

static void setup_string_in_str(struct in_str *i, const char *s)
{
	i->peek = static_peek;
	i->get = static_get;
#if ENABLE_HUSH_INTERACTIVE
	i->promptme = 1;
	i->promptmode = 0; /* PS1 */
#endif
	i->p = s;
	i->eof_flag = 0;
}


/*
 * o_string support
 */
#define B_CHUNK  (32 * sizeof(char*))

static void o_reset(o_string *o)
{
	o->length = 0;
	o->o_quoted = 0;
	if (o->data)
		o->data[0] = '\0';
}

static void o_free(o_string *o)
{
	free(o->data);
	memset(o, 0, sizeof(*o));
}

static ALWAYS_INLINE void o_free_unsafe(o_string *o)
{
	free(o->data);
}

static void o_grow_by(o_string *o, int len)
{
	if (o->length + len > o->maxlen) {
		o->maxlen += (2*len > B_CHUNK ? 2*len : B_CHUNK);
		o->data = xrealloc(o->data, 1 + o->maxlen);
	}
}

static void o_addchr(o_string *o, int ch)
{
	debug_printf("o_addchr: '%c' o->length=%d o=%p\n", ch, o->length, o);
	o_grow_by(o, 1);
	o->data[o->length] = ch;
	o->length++;
	o->data[o->length] = '\0';
}

static void o_addblock(o_string *o, const char *str, int len)
{
	o_grow_by(o, len);
	memcpy(&o->data[o->length], str, len);
	o->length += len;
	o->data[o->length] = '\0';
}

#if !BB_MMU
static void o_addstr(o_string *o, const char *str)
{
	o_addblock(o, str, strlen(str));
}
static void nommu_addchr(o_string *o, int ch)
{
	if (o)
		o_addchr(o, ch);
}
#else
#define nommu_addchr(o, str) ((void)0)
#endif

static void o_addstr_with_NUL(o_string *o, const char *str)
{
	o_addblock(o, str, strlen(str) + 1);
}

static void o_addblock_duplicate_backslash(o_string *o, const char *str, int len)
{
	while (len) {
		o_addchr(o, *str);
		if (*str++ == '\\'
		 && (*str != '*' && *str != '?' && *str != '[')
		) {
			o_addchr(o, '\\');
		}
		len--;
	}
}

/* My analysis of quoting semantics tells me that state information
 * is associated with a destination, not a source.
 */
static void o_addqchr(o_string *o, int ch)
{
	int sz = 1;
	char *found = strchr("*?[\\", ch);
	if (found)
		sz++;
	o_grow_by(o, sz);
	if (found) {
		o->data[o->length] = '\\';
		o->length++;
	}
	o->data[o->length] = ch;
	o->length++;
	o->data[o->length] = '\0';
}

static void o_addQchr(o_string *o, int ch)
{
	int sz = 1;
	if (o->o_escape && strchr("*?[\\", ch)) {
		sz++;
		o->data[o->length] = '\\';
		o->length++;
	}
	o_grow_by(o, sz);
	o->data[o->length] = ch;
	o->length++;
	o->data[o->length] = '\0';
}

static void o_addQstr(o_string *o, const char *str, int len)
{
	if (!o->o_escape) {
		o_addblock(o, str, len);
		return;
	}
	while (len) {
		char ch;
		int sz;
		int ordinary_cnt = strcspn(str, "*?[\\");
		if (ordinary_cnt > len) /* paranoia */
			ordinary_cnt = len;
		o_addblock(o, str, ordinary_cnt);
		if (ordinary_cnt == len)
			return;
		str += ordinary_cnt;
		len -= ordinary_cnt + 1; /* we are processing + 1 char below */

		ch = *str++;
		sz = 1;
		if (ch) { /* it is necessarily one of "*?[\\" */
			sz++;
			o->data[o->length] = '\\';
			o->length++;
		}
		o_grow_by(o, sz);
		o->data[o->length] = ch;
		o->length++;
		o->data[o->length] = '\0';
	}
}

/* A special kind of o_string for $VAR and `cmd` expansion.
 * It contains char* list[] at the beginning, which is grown in 16 element
 * increments. Actual string data starts at the next multiple of 16 * (char*).
 * list[i] contains an INDEX (int!) into this string data.
 * It means that if list[] needs to grow, data needs to be moved higher up
 * but list[i]'s need not be modified.
 * NB: remembering how many list[i]'s you have there is crucial.
 * o_finalize_list() operation post-processes this structure - calculates
 * and stores actual char* ptrs in list[]. Oh, it NULL terminates it as well.
 */
#if DEBUG_EXPAND || DEBUG_GLOB
static void debug_print_list(const char *prefix, o_string *o, int n)
{
	char **list = (char**)o->data;
	int string_start = ((n + 0xf) & ~0xf) * sizeof(list[0]);
	int i = 0;
	fprintf(stderr, "%s: list:%p n:%d string_start:%d length:%d maxlen:%d\n",
			prefix, list, n, string_start, o->length, o->maxlen);
	while (i < n) {
		fprintf(stderr, " list[%d]=%d '%s' %p\n", i, (int)list[i],
				o->data + (int)list[i] + string_start,
				o->data + (int)list[i] + string_start);
		i++;
	}
	if (n) {
		const char *p = o->data + (int)list[n - 1] + string_start;
		fprintf(stderr, " total_sz:%ld\n", (long)((p + strlen(p) + 1) - o->data));
	}
}
#else
#define debug_print_list(prefix, o, n) ((void)0)
#endif

/* n = o_save_ptr_helper(str, n) "starts new string" by storing an index value
 * in list[n] so that it points past last stored byte so far.
 * It returns n+1. */
static int o_save_ptr_helper(o_string *o, int n)
{
	char **list = (char**)o->data;
	int string_start;
	int string_len;

	if (!o->has_empty_slot) {
		string_start = ((n + 0xf) & ~0xf) * sizeof(list[0]);
		string_len = o->length - string_start;
		if (!(n & 0xf)) { /* 0, 0x10, 0x20...? */
			debug_printf_list("list[%d]=%d string_start=%d (growing)\n", n, string_len, string_start);
			/* list[n] points to string_start, make space for 16 more pointers */
			o->maxlen += 0x10 * sizeof(list[0]);
			o->data = xrealloc(o->data, o->maxlen + 1);
			list = (char**)o->data;
			memmove(list + n + 0x10, list + n, string_len);
			o->length += 0x10 * sizeof(list[0]);
		} else {
			debug_printf_list("list[%d]=%d string_start=%d\n",
					n, string_len, string_start);
		}
	} else {
		/* We have empty slot at list[n], reuse without growth */
		string_start = ((n+1 + 0xf) & ~0xf) * sizeof(list[0]); /* NB: n+1! */
		string_len = o->length - string_start;
		debug_printf_list("list[%d]=%d string_start=%d (empty slot)\n",
				n, string_len, string_start);
		o->has_empty_slot = 0;
	}
	list[n] = (char*)(ptrdiff_t)string_len;
	return n + 1;
}

/* "What was our last o_save_ptr'ed position (byte offset relative o->data)?" */
static int o_get_last_ptr(o_string *o, int n)
{
	char **list = (char**)o->data;
	int string_start = ((n + 0xf) & ~0xf) * sizeof(list[0]);

	return ((int)(ptrdiff_t)list[n-1]) + string_start;
}

/* o_glob performs globbing on last list[], saving each result
 * as a new list[]. */
static int o_glob(o_string *o, int n)
{
	glob_t globdata;
	int gr;
	char *pattern;

	debug_printf_glob("start o_glob: n:%d o->data:%p\n", n, o->data);
	if (!o->data)
		return o_save_ptr_helper(o, n);
	pattern = o->data + o_get_last_ptr(o, n);
	debug_printf_glob("glob pattern '%s'\n", pattern);
	if (!glob_needed(pattern)) {
 literal:
		o->length = unbackslash(pattern) - o->data;
		debug_printf_glob("glob pattern '%s' is literal\n", pattern);
		return o_save_ptr_helper(o, n);
	}

	memset(&globdata, 0, sizeof(globdata));
	gr = glob(pattern, 0, NULL, &globdata);
	debug_printf_glob("glob('%s'):%d\n", pattern, gr);
	if (gr == GLOB_NOSPACE)
		bb_error_msg_and_die("out of memory during glob");
	if (gr == GLOB_NOMATCH) {
		globfree(&globdata);
		goto literal;
	}
	if (gr != 0) { /* GLOB_ABORTED ? */
//TODO: testcase for bad glob pattern behavior
		bb_error_msg("glob(3) error %d on '%s'", gr, pattern);
	}
	if (globdata.gl_pathv && globdata.gl_pathv[0]) {
		char **argv = globdata.gl_pathv;
		o->length = pattern - o->data; /* "forget" pattern */
		while (1) {
			o_addstr_with_NUL(o, *argv);
			n = o_save_ptr_helper(o, n);
			argv++;
			if (!*argv)
				break;
		}
	}
	globfree(&globdata);
	if (DEBUG_GLOB)
		debug_print_list("o_glob returning", o, n);
	return n;
}

/* If o->o_glob == 1, glob the string so far remembered.
 * Otherwise, just finish current list[] and start new */
static int o_save_ptr(o_string *o, int n)
{
	if (o->o_glob) { /* if globbing is requested */
		/* If o->has_empty_slot, list[n] was already globbed
		 * (if it was requested back then when it was filled)
		 * so don't do that again! */
		if (!o->has_empty_slot)
			return o_glob(o, n); /* o_save_ptr_helper is inside */
	}
	return o_save_ptr_helper(o, n);
}

/* "Please convert list[n] to real char* ptrs, and NULL terminate it." */
static char **o_finalize_list(o_string *o, int n)
{
	char **list;
	int string_start;

	n = o_save_ptr(o, n); /* force growth for list[n] if necessary */
	if (DEBUG_EXPAND)
		debug_print_list("finalized", o, n);
	debug_printf_expand("finalized n:%d\n", n);
	list = (char**)o->data;
	string_start = ((n + 0xf) & ~0xf) * sizeof(list[0]);
	list[--n] = NULL;
	while (n) {
		n--;
		list[n] = o->data + (int)(ptrdiff_t)list[n] + string_start;
	}
	return list;
}


/* Expansion can recurse */
#if ENABLE_HUSH_TICK
static int process_command_subs(o_string *dest, const char *s);
#endif
static char *expand_string_to_string(const char *str);
#if BB_MMU
#define parse_stream_dquoted(as_string, dest, input, dquote_end) \
	parse_stream_dquoted(dest, input, dquote_end)
#endif
static int parse_stream_dquoted(o_string *as_string,
		o_string *dest,
		struct in_str *input,
		int dquote_end);

/* expand_strvec_to_strvec() takes a list of strings, expands
 * all variable references within and returns a pointer to
 * a list of expanded strings, possibly with larger number
 * of strings. (Think VAR="a b"; echo $VAR).
 * This new list is allocated as a single malloc block.
 * NULL-terminated list of char* pointers is at the beginning of it,
 * followed by strings themself.
 * Caller can deallocate entire list by single free(list). */

/* Store given string, finalizing the word and starting new one whenever
 * we encounter IFS char(s). This is used for expanding variable values.
 * End-of-string does NOT finalize word: think about 'echo -$VAR-' */
static int expand_on_ifs(o_string *output, int n, const char *str)
{
	while (1) {
		int word_len = strcspn(str, G.ifs);
		if (word_len) {
			if (output->o_escape || !output->o_glob)
				o_addQstr(output, str, word_len);
			else /* protect backslashes against globbing up :) */
				o_addblock_duplicate_backslash(output, str, word_len);
			str += word_len;
		}
		if (!*str)  /* EOL - do not finalize word */
			break;
		o_addchr(output, '\0');
		debug_print_list("expand_on_ifs", output, n);
		n = o_save_ptr(output, n);
		str += strspn(str, G.ifs); /* skip ifs chars */
	}
	debug_print_list("expand_on_ifs[1]", output, n);
	return n;
}

/* Helper to expand $((...)) and heredoc body. These act as if
 * they are in double quotes, with the exception that they are not :).
 * Just the rules are similar: "expand only $var and `cmd`"
 *
 * Returns malloced string.
 * As an optimization, we return NULL if expansion is not needed.
 */
static char *expand_pseudo_dquoted(const char *str)
{
	char *exp_str;
	struct in_str input;
	o_string dest = NULL_O_STRING;

	if (strchr(str, '$') == NULL
#if ENABLE_HUSH_TICK
	 && strchr(str, '`') == NULL
#endif
	) {
		return NULL;
	}

	/* We need to expand. Example:
	 * echo $(($a + `echo 1`)) $((1 + $((2)) ))
	 */
	setup_string_in_str(&input, str);
	parse_stream_dquoted(NULL, &dest, &input, EOF);
	//bb_error_msg("'%s' -> '%s'", str, dest.data);
	exp_str = expand_string_to_string(dest.data);
	//bb_error_msg("'%s' -> '%s'", dest.data, exp_str);
	o_free_unsafe(&dest);
	return exp_str;
}

/* Expand all variable references in given string, adding words to list[]
 * at n, n+1,... positions. Return updated n (so that list[n] is next one
 * to be filled). This routine is extremely tricky: has to deal with
 * variables/parameters with whitespace, $* and $@, and constructs like
 * 'echo -$*-'. If you play here, you must run testsuite afterwards! */
static int expand_vars_to_list(o_string *output, int n, char *arg, char or_mask)
{
	/* or_mask is either 0 (normal case) or 0x80
	 * (expansion of right-hand side of assignment == 1-element expand.
	 * It will also do no globbing, and thus we must not backslash-quote!) */

	char first_ch, ored_ch;
	int i;
	const char *val;
	char *dyn_val, *p;

	dyn_val = NULL;
	ored_ch = 0;

	debug_printf_expand("expand_vars_to_list: arg '%s'\n", arg);
	debug_print_list("expand_vars_to_list", output, n);
	n = o_save_ptr(output, n);
	debug_print_list("expand_vars_to_list[0]", output, n);

	while ((p = strchr(arg, SPECIAL_VAR_SYMBOL)) != NULL) {
#if ENABLE_HUSH_TICK
		o_string subst_result = NULL_O_STRING;
#endif
#if ENABLE_SH_MATH_SUPPORT
		char arith_buf[sizeof(arith_t)*3 + 2];
#endif
		o_addblock(output, arg, p - arg);
		debug_print_list("expand_vars_to_list[1]", output, n);
		arg = ++p;
		p = strchr(p, SPECIAL_VAR_SYMBOL);

		first_ch = arg[0] | or_mask; /* forced to "quoted" if or_mask = 0x80 */
		/* "$@" is special. Even if quoted, it can still
		 * expand to nothing (not even an empty string) */
		if ((first_ch & 0x7f) != '@')
			ored_ch |= first_ch;

		val = NULL;
		switch (first_ch & 0x7f) {
		/* Highest bit in first_ch indicates that var is double-quoted */
		case '$': /* pid */
			val = utoa(G.root_pid);
			break;
		case '!': /* bg pid */
			val = G.last_bg_pid ? utoa(G.last_bg_pid) : (char*)"";
			break;
		case '?': /* exitcode */
			val = utoa(G.last_exitcode);
			break;
		case '#': /* argc */
			if (arg[1] != SPECIAL_VAR_SYMBOL)
				/* actually, it's a ${#var} */
				goto case_default;
			val = utoa(G.global_argc ? G.global_argc-1 : 0);
			break;
		case '*':
		case '@':
			i = 1;
			if (!G.global_argv[i])
				break;
			ored_ch |= first_ch; /* do it for "$@" _now_, when we know it's not empty */
			if (!(first_ch & 0x80)) { /* unquoted $* or $@ */
				smallint sv = output->o_escape;
				/* unquoted var's contents should be globbed, so don't escape */
				output->o_escape = 0;
				while (G.global_argv[i]) {
					n = expand_on_ifs(output, n, G.global_argv[i]);
					debug_printf_expand("expand_vars_to_list: argv %d (last %d)\n", i, G.global_argc - 1);
					if (G.global_argv[i++][0] && G.global_argv[i]) {
						/* this argv[] is not empty and not last:
						 * put terminating NUL, start new word */
						o_addchr(output, '\0');
						debug_print_list("expand_vars_to_list[2]", output, n);
						n = o_save_ptr(output, n);
						debug_print_list("expand_vars_to_list[3]", output, n);
					}
				}
				output->o_escape = sv;
			} else
			/* If or_mask is nonzero, we handle assignment 'a=....$@.....'
			 * and in this case should treat it like '$*' - see 'else...' below */
			if (first_ch == ('@'|0x80) && !or_mask) { /* quoted $@ */
				while (1) {
					o_addQstr(output, G.global_argv[i], strlen(G.global_argv[i]));
					if (++i >= G.global_argc)
						break;
					o_addchr(output, '\0');
					debug_print_list("expand_vars_to_list[4]", output, n);
					n = o_save_ptr(output, n);
				}
			} else { /* quoted $*: add as one word */
				while (1) {
					o_addQstr(output, G.global_argv[i], strlen(G.global_argv[i]));
					if (!G.global_argv[++i])
						break;
					if (G.ifs[0])
						o_addchr(output, G.ifs[0]);
				}
			}
			break;
		case SPECIAL_VAR_SYMBOL: /* <SPECIAL_VAR_SYMBOL><SPECIAL_VAR_SYMBOL> */
			/* "Empty variable", used to make "" etc to not disappear */
			arg++;
			ored_ch = 0x80;
			break;
#if ENABLE_HUSH_TICK
		case '`': /* <SPECIAL_VAR_SYMBOL>`cmd<SPECIAL_VAR_SYMBOL> */
			*p = '\0';
			arg++;
//TODO: can we just stuff it into "output" directly?
			debug_printf_subst("SUBST '%s' first_ch %x\n", arg, first_ch);
			process_command_subs(&subst_result, arg);
			debug_printf_subst("SUBST RES '%s'\n", subst_result.data);
			val = subst_result.data;
			goto store_val;
#endif
#if ENABLE_SH_MATH_SUPPORT
		case '+': { /* <SPECIAL_VAR_SYMBOL>+cmd<SPECIAL_VAR_SYMBOL> */
			arith_eval_hooks_t hooks;
			arith_t res;
			int errcode;
			char *exp_str;

			arg++; /* skip '+' */
			*p = '\0'; /* replace trailing <SPECIAL_VAR_SYMBOL> */
			debug_printf_subst("ARITH '%s' first_ch %x\n", arg, first_ch);

			exp_str = expand_pseudo_dquoted(arg);
			hooks.lookupvar = get_local_var_value;
			hooks.setvar = arith_set_local_var;
			hooks.endofname = endofname;
			res = arith(exp_str ? exp_str : arg, &errcode, &hooks);
			free(exp_str);

			if (errcode < 0) {
				switch (errcode) {
				case -3: maybe_die("arith", "exponent less than 0"); break;
				case -2: maybe_die("arith", "divide by zero"); break;
				case -5: maybe_die("arith", "expression recursion loop detected"); break;
				default: maybe_die("arith", "syntax error"); break;
				}
			}
			debug_printf_subst("ARITH RES '"arith_t_fmt"'\n", res);
			sprintf(arith_buf, arith_t_fmt, res);
			val = arith_buf;
			break;
		}
#endif
		default: /* <SPECIAL_VAR_SYMBOL>varname<SPECIAL_VAR_SYMBOL> */
		case_default: {
			bool exp_len = false;
			bool exp_null = false;
			char *var = arg;
			char exp_save = exp_save; /* for compiler */
			char exp_op = exp_op; /* for compiler */
			char *exp_word = exp_word; /* for compiler */
			size_t exp_off = 0;

			*p = '\0';
			arg[0] = first_ch & 0x7f;

			/* prepare for expansions */
			if (var[0] == '#') {
				/* handle length expansion ${#var} */
				exp_len = true;
				++var;
			} else {
				/* maybe handle parameter expansion */
				exp_off = strcspn(var, ":-=+?%#");
				if (!var[exp_off])
					exp_off = 0;
				if (exp_off) {
					exp_save = var[exp_off];
					exp_null = exp_save == ':';
					exp_word = var + exp_off;
					if (exp_null) ++exp_word;
					exp_op = *exp_word++;
					var[exp_off] = '\0';
				}
			}

			/* lookup the variable in question */
			if (isdigit(var[0])) {
				/* handle_dollar() should have vetted var for us */
				i = xatoi_u(var);
				if (i < G.global_argc)
					val = G.global_argv[i];
				/* else val remains NULL: $N with too big N */
			} else
				val = get_local_var_value(var);

			/* handle any expansions */
			if (exp_len) {
				debug_printf_expand("expand: length of '%s' = ", val);
				val = utoa(val ? strlen(val) : 0);
				debug_printf_expand("%s\n", val);
			} else if (exp_off) {
				if (exp_op == '%' || exp_op == '#') {
					/* we need to do a pattern match */
					bool zero;
					char *loc;
					scan_t scan = pick_scan(exp_op, *exp_word, &zero);
					if (exp_op == *exp_word)	/* ## or %% */
						++exp_word;
					val = dyn_val = xstrdup(val);
					loc = scan(dyn_val, exp_word, zero);
					if (zero)
						val = loc;
					else
						*loc = '\0';
				} else {
					/* we need to do an expansion */
					int exp_test = (!val || (exp_null && !val[0]));
					if (exp_op == '+')
						exp_test = !exp_test;
					debug_printf_expand("expand: op:%c (null:%s) test:%i\n", exp_op,
						exp_null ? "true" : "false", exp_test);
					if (exp_test) {
						if (exp_op == '?')
							maybe_die(var, *exp_word ? exp_word : "parameter null or not set");
						else
							val = exp_word;

						if (exp_op == '=') {
							if (isdigit(var[0]) || var[0] == '#') {
								maybe_die(var, "special vars cannot assign in this way");
								val = NULL;
							} else {
								char *new_var = xmalloc(strlen(var) + strlen(val) + 2);
								sprintf(new_var, "%s=%s", var, val);
								set_local_var(new_var, -1, 0);
							}
						}
					}
				}

				var[exp_off] = exp_save;
			}

			arg[0] = first_ch;
#if ENABLE_HUSH_TICK
 store_val:
#endif
			if (!(first_ch & 0x80)) { /* unquoted $VAR */
				debug_printf_expand("unquoted '%s', output->o_escape:%d\n", val, output->o_escape);
				if (val) {
					/* unquoted var's contents should be globbed, so don't escape */
					smallint sv = output->o_escape;
					output->o_escape = 0;
					n = expand_on_ifs(output, n, val);
					val = NULL;
					output->o_escape = sv;
				}
			} else { /* quoted $VAR, val will be appended below */
				debug_printf_expand("quoted '%s', output->o_escape:%d\n", val, output->o_escape);
			}
		} /* default: */
		} /* switch (char after <SPECIAL_VAR_SYMBOL>) */
		if (val) {
			o_addQstr(output, val, strlen(val));
		}
		free(dyn_val);
		dyn_val = NULL;
		/* Do the check to avoid writing to a const string */
		if (*p != SPECIAL_VAR_SYMBOL)
			*p = SPECIAL_VAR_SYMBOL;

#if ENABLE_HUSH_TICK
		o_free(&subst_result);
#endif
		arg = ++p;
	} /* end of "while (SPECIAL_VAR_SYMBOL is found) ..." */

	if (arg[0]) {
		debug_print_list("expand_vars_to_list[a]", output, n);
		/* this part is literal, and it was already pre-quoted
		 * if needed (much earlier), do not use o_addQstr here! */
		o_addstr_with_NUL(output, arg);
		debug_print_list("expand_vars_to_list[b]", output, n);
	} else if (output->length == o_get_last_ptr(output, n) /* expansion is empty */
	 && !(ored_ch & 0x80) /* and all vars were not quoted. */
	) {
		n--;
		/* allow to reuse list[n] later without re-growth */
		output->has_empty_slot = 1;
	} else {
		o_addchr(output, '\0');
	}
	return n;
}

static char **expand_variables(char **argv, int or_mask)
{
	int n;
	char **list;
	char **v;
	o_string output = NULL_O_STRING;

	if (or_mask & 0x100) {
		output.o_escape = 1; /* protect against globbing for "$var" */
		/* (unquoted $var will temporarily switch it off) */
		output.o_glob = 1;
	}

	n = 0;
	v = argv;
	while (*v) {
		n = expand_vars_to_list(&output, n, *v, (char)or_mask);
		v++;
	}
	debug_print_list("expand_variables", &output, n);

	/* output.data (malloced in one block) gets returned in "list" */
	list = o_finalize_list(&output, n);
	debug_print_strings("expand_variables[1]", list);
	return list;
}

static char **expand_strvec_to_strvec(char **argv)
{
	return expand_variables(argv, 0x100);
}

/* Used for expansion of right hand of assignments */
/* NB: should NOT do globbing! "export v=/bin/c*; env | grep ^v=" outputs
 * "v=/bin/c*" */
static char *expand_string_to_string(const char *str)
{
	char *argv[2], **list;

	argv[0] = (char*)str;
	argv[1] = NULL;
	list = expand_variables(argv, 0x80); /* 0x80: make one-element expansion */
	if (HUSH_DEBUG)
		if (!list[0] || list[1])
			bb_error_msg_and_die("BUG in varexp2");
	/* actually, just move string 2*sizeof(char*) bytes back */
	overlapping_strcpy((char*)list, list[0]);
	debug_printf_expand("string_to_string='%s'\n", (char*)list);
	return (char*)list;
}

/* Used for "eval" builtin */
static char* expand_strvec_to_string(char **argv)
{
	char **list;

	list = expand_variables(argv, 0x80);
	/* Convert all NULs to spaces */
	if (list[0]) {
		int n = 1;
		while (list[n]) {
			if (HUSH_DEBUG)
				if (list[n-1] + strlen(list[n-1]) + 1 != list[n])
					bb_error_msg_and_die("BUG in varexp3");
			list[n][-1] = ' '; /* TODO: or to G.ifs[0]? */
			n++;
		}
	}
	overlapping_strcpy((char*)list, list[0]);
	debug_printf_expand("strvec_to_string='%s'\n", (char*)list);
	return (char*)list;
}

static char **expand_assignments(char **argv, int count)
{
	int i;
	char **p = NULL;
	/* Expand assignments into one string each */
	for (i = 0; i < count; i++) {
		p = add_string_to_strings(p, expand_string_to_string(argv[i]));
	}
	return p;
}


#if BB_MMU
void re_execute_shell(const char *s, int is_heredoc); /* never called */
#define clean_up_after_re_execute() ((void)0)
static void reset_traps_to_defaults(void)
{
	unsigned sig;
	int dirty;

	if (!G.traps)
		return;
	dirty = 0;
	for (sig = 0; sig < NSIG; sig++) {
		if (!G.traps[sig])
			continue;
		free(G.traps[sig]);
		G.traps[sig] = NULL;
		/* There is no signal for 0 (EXIT) */
		if (sig == 0)
			continue;
		/* there was a trap handler, we are removing it
		 * (if sig has non-DFL handling,
		 * we don't need to do anything) */
		if (sig < 32 && (G.non_DFL_mask & (1 << sig)))
			continue;
		sigdelset(&G.blocked_set, sig);
		dirty = 1;
	}
	if (dirty)
		sigprocmask(SIG_SETMASK, &G.blocked_set, NULL);
}

#else /* !BB_MMU */

static void re_execute_shell(const char *s, int is_heredoc) NORETURN;
static void re_execute_shell(const char *s, int is_heredoc)
{
	char param_buf[sizeof("-$%x:%x:%x:%x") + sizeof(unsigned) * 4];
	char *heredoc_argv[4];
	struct variable *cur;
	char **argv, **pp, **pp2;
	unsigned cnt;

	if (is_heredoc) {
		argv = heredoc_argv;
		argv[0] = (char *) G.argv0_for_re_execing;
		argv[1] = (char *) "-<";
		argv[2] = (char *) s;
		argv[3] = NULL;
		pp = &argv[3]; /* used as pointer to empty environment */
		goto do_exec;
	}

	sprintf(param_buf, "-$%x:%x:%x" USE_HUSH_LOOPS(":%x")
			, (unsigned) G.root_pid
			, (unsigned) G.last_bg_pid
			, (unsigned) G.last_exitcode
			USE_HUSH_LOOPS(, G.depth_of_loop)
			);
	/* 1:hush 2:-$<pid>:<pid>:<exitcode>:<depth> <vars...>
	 * 3:-c 4:<cmd> <argN...> 5:NULL
	 */
	cnt = 5 + G.global_argc;
	for (cur = G.top_var; cur; cur = cur->next) {
		if (!cur->flg_export || cur->flg_read_only)
			cnt += 2;
	}
	G.argv_from_re_execing = argv = pp = xzalloc(sizeof(argv[0]) * cnt);
	*pp++ = (char *) G.argv0_for_re_execing;
	*pp++ = param_buf;
	for (cur = G.top_var; cur; cur = cur->next) {
		if (cur->varstr == hush_version_str)
			continue;
		if (cur->flg_read_only) {
			*pp++ = (char *) "-R";
			*pp++ = cur->varstr;
		} else if (!cur->flg_export) {
			*pp++ = (char *) "-V";
			*pp++ = cur->varstr;
		}
	}
//TODO: pass functions
	/* We can pass activated traps here. Say, -Tnn:trap_string
	 *
	 * However, POSIX says that subshells reset signals with traps
	 * to SIG_DFL.
	 * I tested bash-3.2 and it not only does that with true subshells
	 * of the form ( list ), but with any forked children shells.
	 * I set trap "echo W" WINCH; and then tried:
	 *
	 * { echo 1; sleep 20; echo 2; } &
	 * while true; do echo 1; sleep 20; echo 2; break; done &
	 * true | { echo 1; sleep 20; echo 2; } | cat
	 *
	 * In all these cases sending SIGWINCH to the child shell
	 * did not run the trap. If I add trap "echo V" WINCH;
	 * _inside_ group (just before echo 1), it works.
	 *
	 * I conclude it means we don't need to pass active traps here.
	 * exec syscall below resets them to SIG_DFL for us.
	 */
	*pp++ = (char *) "-c";
	*pp++ = (char *) s;
	pp2 = G.global_argv;
	while (*pp2)
		*pp++ = *pp2++;
	/* *pp = NULL; - is already there */
	pp = environ;

 do_exec:
	debug_printf_exec("re_execute_shell pid:%d cmd:'%s'\n", getpid(), s);
	sigprocmask(SIG_SETMASK, &G.inherited_set, NULL);
	execve(bb_busybox_exec_path, argv, pp);
	/* Fallback. Useful for init=/bin/hush usage etc */
	if (argv[0][0] == '/')
		execve(argv[0], argv, pp);
	xfunc_error_retval = 127;
	bb_error_msg_and_die("can't re-execute the shell");
}

static void clean_up_after_re_execute(void)
{
	char **pp = G.argv_from_re_execing;
	if (pp) {
		/* Must match re_execute_shell's allocations (if any) */
		free(pp);
		G.argv_from_re_execing = NULL;
	}
}
#endif  /* !BB_MMU */


static void setup_heredoc(struct redir_struct *redir)
{
	struct fd_pair pair;
	pid_t pid;
	int len, written;
	/* the _body_ of heredoc (misleading field name) */
	const char *heredoc = redir->rd_filename;
	char *expanded;

	expanded = NULL;
	if (!(redir->rd_dup & HEREDOC_QUOTED)) {
		expanded = expand_pseudo_dquoted(heredoc);
		if (expanded)
			heredoc = expanded;
	}
	len = strlen(heredoc);

	close(redir->rd_fd); /* often saves dup2+close in xmove_fd */
	xpiped_pair(pair);
	xmove_fd(pair.rd, redir->rd_fd);

	/* Try writing without forking. Newer kernels have
	 * dynamically growing pipes. Must use non-blocking write! */
	ndelay_on(pair.wr);
	while (1) {
		written = write(pair.wr, heredoc, len);
		if (written <= 0)
			break;
		len -= written;
		if (len == 0) {
			close(pair.wr);
			return;
		}
		heredoc += written;
	}
	ndelay_off(pair.wr);

	/* Okay, pipe buffer was not big enough */
	/* Note: we must not create a stray child (bastard? :)
	 * for the unsuspecting parent process. Child creates a grandchild
	 * and exits before parent execs the process which consumes heredoc
	 * (that exec happens after we return from this function) */
	pid = vfork();
	if (pid < 0)
		bb_perror_msg_and_die("vfork");
	if (pid == 0) {
		/* child */
		pid = BB_MMU ? fork() : vfork();
		if (pid < 0)
			bb_perror_msg_and_die(BB_MMU ? "fork" : "vfork");
		if (pid != 0)
			_exit(0);
		/* grandchild */
		close(redir->rd_fd); /* read side of the pipe */
#if BB_MMU
		full_write(pair.wr, heredoc, len); /* may loop or block */
		_exit(0);
#else
		/* Delegate blocking writes to another process */
		disable_restore_tty_pgrp_on_exit();
		xmove_fd(pair.wr, STDOUT_FILENO);
		re_execute_shell(heredoc, 1);
#endif
	}
	/* parent */
	enable_restore_tty_pgrp_on_exit();
	clean_up_after_re_execute();
	close(pair.wr);
	free(expanded);
	wait(NULL); /* wait till child has died */
}

/* squirrel != NULL means we squirrel away copies of stdin, stdout,
 * and stderr if they are redirected. */
static int setup_redirects(struct command *prog, int squirrel[])
{
//TODO: no callers ever check return value - ?!
	int openfd, mode;
	struct redir_struct *redir;

	for (redir = prog->redirects; redir; redir = redir->next) {
		if (redir->rd_type == REDIRECT_HEREDOC2) {
			if (squirrel && redir->rd_fd < 3) {
				squirrel[redir->rd_fd] = dup(redir->rd_fd);
			}
			/* for REDIRECT_HEREDOC2, rd_filename holds _contents_
			 * of the heredoc */
			debug_printf_parse("set heredoc '%s'\n",
					redir->rd_filename);
			setup_heredoc(redir);
			continue;
		}

		if (redir->rd_dup == -1) {
			char *p;
			if (redir->rd_filename == NULL) {
				/* Something went wrong in the parse.
				 * Pretend it didn't happen */
				continue;
			}
			mode = redir_table[redir->rd_type].mode;
//TODO: check redir for names like '\\'
			p = expand_string_to_string(redir->rd_filename);
			openfd = open_or_warn(p, mode);
			free(p);
			if (openfd < 0) {
			/* this could get lost if stderr has been redirected, but
			 * bash and ash both lose it as well (though zsh doesn't!) */
//what the above comment tries to say?
				return 1;
			}
		} else {
			openfd = redir->rd_dup;
		}

		if (openfd != redir->rd_fd) {
			if (squirrel && redir->rd_fd < 3) {
				squirrel[redir->rd_fd] = dup(redir->rd_fd);
			}
			if (openfd == REDIRFD_CLOSE) {
				/* "n>-" means "close me" */
				close(redir->rd_fd);
			} else {
				xdup2(openfd, redir->rd_fd);
				if (redir->rd_dup == -1)
					close(openfd);
			}
		}
	}
	return 0;
}

static void restore_redirects(int squirrel[])
{
	int i, fd;
	for (i = 0; i < 3; i++) {
		fd = squirrel[i];
		if (fd != -1) {
			/* We simply die on error */
			xmove_fd(fd, i);
		}
	}
}


#if !DEBUG_CLEAN
#define free_pipe_list(head, indent) free_pipe_list(head)
#define free_pipe(pi, indent)        free_pipe(pi)
#endif
static void free_pipe_list(struct pipe *head, int indent);

/* Return code is the exit status of the pipe */
static void free_pipe(struct pipe *pi, int indent)
{
	char **p;
	struct command *command;
	struct redir_struct *r, *rnext;
	int a, i;

	if (pi->stopped_cmds > 0) /* why? */
		return;
	debug_printf_clean("%s run pipe: (pid %d)\n", indenter(indent), getpid());
	for (i = 0; i < pi->num_cmds; i++) {
		command = &pi->cmds[i];
		debug_printf_clean("%s  command %d:\n", indenter(indent), i);
		if (command->argv) {
			for (a = 0, p = command->argv; *p; a++, p++) {
				debug_printf_clean("%s   argv[%d] = %s\n",
						indenter(indent), a, *p);
			}
			free_strings(command->argv);
			command->argv = NULL;
		}
		/* not "else if": on syntax error, we may have both! */
		if (command->group) {
			debug_printf_clean("%s   begin group (grp_type:%d)\n",
					indenter(indent), command->grp_type);
			free_pipe_list(command->group, indent+3);
			debug_printf_clean("%s   end group\n", indenter(indent));
			command->group = NULL;
		}
#if !BB_MMU
		free(command->group_as_string);
		command->group_as_string = NULL;
#endif
		for (r = command->redirects; r; r = rnext) {
			debug_printf_clean("%s   redirect %d%s", indenter(indent),
					r->fd, redir_table[r->rd_type].descrip);
			/* guard against the case >$FOO, where foo is unset or blank */
			if (r->rd_filename) {
				debug_printf_clean(" fname:'%s'\n", r->rd_filename);
				free(r->rd_filename);
				r->rd_filename = NULL;
			}
			debug_printf_clean(" rd_dup:%d\n", r->rd_dup);
			rnext = r->next;
			free(r);
		}
		command->redirects = NULL;
	}
	free(pi->cmds);   /* children are an array, they get freed all at once */
	pi->cmds = NULL;
#if ENABLE_HUSH_JOB
	free(pi->cmdtext);
	pi->cmdtext = NULL;
#endif
}

static void free_pipe_list(struct pipe *head, int indent)
{
	struct pipe *pi, *next;

	for (pi = head; pi; pi = next) {
#if HAS_KEYWORDS
		debug_printf_clean("%s pipe reserved word %d\n", indenter(indent), pi->res_word);
#endif
		free_pipe(pi, indent);
		debug_printf_clean("%s pipe followup code %d\n", indenter(indent), pi->followup);
		next = pi->next;
		/*pi->next = NULL;*/
		free(pi);
	}
}


#if BB_MMU
#define pseudo_exec_argv(nommu_save, argv, assignment_cnt, argv_expanded) \
	pseudo_exec_argv(argv, assignment_cnt, argv_expanded)
#define pseudo_exec(nommu_save, command, argv_expanded) \
	pseudo_exec(command, argv_expanded)
#endif

/* Called after [v]fork() in run_pipe, or from builtin_exec.
 * Never returns.
 * XXX no exit() here.  If you don't exec, use _exit instead.
 * The at_exit handlers apparently confuse the calling process,
 * in particular stdin handling.  Not sure why? -- because of vfork! (vda) */
static void pseudo_exec_argv(nommu_save_t *nommu_save,
		char **argv, int assignment_cnt,
		char **argv_expanded) NORETURN;
static void pseudo_exec_argv(nommu_save_t *nommu_save,
		char **argv, int assignment_cnt,
		char **argv_expanded)
{
	char **new_env;

	/* Case when we are here: ... | var=val | ... */
	if (!argv[assignment_cnt])
		_exit(EXIT_SUCCESS);

	new_env = expand_assignments(argv, assignment_cnt);
#if BB_MMU
	putenv_all(new_env);
	free(new_env); /* optional */
#else
	nommu_save->new_env = new_env;
	nommu_save->old_env = putenv_all_and_save_old(new_env);
#endif
	if (argv_expanded) {
		argv = argv_expanded;
	} else {
		argv = expand_strvec_to_strvec(argv + assignment_cnt);
#if !BB_MMU
		nommu_save->argv = argv;
#endif
	}

	/* On NOMMU, we must never block!
	 * Example: { sleep 99999 | read line } & echo Ok
	 * read builtin will block on read syscall, leaving parent blocked
	 * in vfork. Therefore we can't do this:
	 */
#if BB_MMU
	/* Check if the command matches any of the builtins.
	 * Depending on context, this might be redundant.  But it's
	 * easier to waste a few CPU cycles than it is to figure out
	 * if this is one of those cases.
	 */
	{
		int rcode;
		const struct built_in_command *x;
		for (x = bltins; x != &bltins[ARRAY_SIZE(bltins)]; x++) {
			if (strcmp(argv[0], x->cmd) == 0) {
				debug_printf_exec("running builtin '%s'\n",
						argv[0]);
				rcode = x->function(argv);
				fflush(NULL);
				_exit(rcode);
			}
		}
	}
#endif

#if ENABLE_FEATURE_SH_STANDALONE
	/* Check if the command matches any busybox applets */
	if (strchr(argv[0], '/') == NULL) {
		int a = find_applet_by_name(argv[0]);
		if (a >= 0) {
#if BB_MMU /* see above why on NOMMU it is not allowed */
			if (APPLET_IS_NOEXEC(a)) {
				debug_printf_exec("running applet '%s'\n", argv[0]);
				run_applet_no_and_exit(a, argv);
			}
#endif
			/* Re-exec ourselves */
			debug_printf_exec("re-execing applet '%s'\n", argv[0]);
			sigprocmask(SIG_SETMASK, &G.inherited_set, NULL);
			execv(bb_busybox_exec_path, argv);
			/* If they called chroot or otherwise made the binary no longer
			 * executable, fall through */
		}
	}
#endif

	debug_printf_exec("execing '%s'\n", argv[0]);
	sigprocmask(SIG_SETMASK, &G.inherited_set, NULL);
	execvp(argv[0], argv);
	bb_perror_msg("can't exec '%s'", argv[0]);
	_exit(EXIT_FAILURE);
}

static int run_list(struct pipe *pi);

/* Called after [v]fork() in run_pipe
 */
static void pseudo_exec(nommu_save_t *nommu_save,
		struct command *command,
		char **argv_expanded) NORETURN;
static void pseudo_exec(nommu_save_t *nommu_save,
		struct command *command,
		char **argv_expanded)
{
	if (command->argv) {
		pseudo_exec_argv(nommu_save, command->argv,
				command->assignment_cnt, argv_expanded);
	}

	if (command->group) {
		/* Cases when we are here:
		 * ( list )
		 * { list } &
		 * ... | ( list ) | ...
		 * ... | { list } | ...
		 */
#if BB_MMU
		int rcode;
		debug_printf_exec("pseudo_exec: run_list\n");
		reset_traps_to_defaults();
		rcode = run_list(command->group);
		/* OK to leak memory by not calling free_pipe_list,
		 * since this process is about to exit */
		_exit(rcode);
#else
		re_execute_shell(command->group_as_string, 0);
#endif
	}

	/* Case when we are here: ... | >file */
	debug_printf_exec("pseudo_exec'ed null command\n");
	_exit(EXIT_SUCCESS);
}

#if ENABLE_HUSH_JOB
static const char *get_cmdtext(struct pipe *pi)
{
	char **argv;
	char *p;
	int len;

	/* This is subtle. ->cmdtext is created only on first backgrounding.
	 * (Think "cat, <ctrl-z>, fg, <ctrl-z>, fg, <ctrl-z>...." here...)
	 * On subsequent bg argv is trashed, but we won't use it */
	if (pi->cmdtext)
		return pi->cmdtext;
	argv = pi->cmds[0].argv;
	if (!argv || !argv[0]) {
		pi->cmdtext = xzalloc(1);
		return pi->cmdtext;
	}

	len = 0;
	do len += strlen(*argv) + 1; while (*++argv);
	pi->cmdtext = p = xmalloc(len);
	argv = pi->cmds[0].argv;
	do {
		len = strlen(*argv);
		memcpy(p, *argv, len);
		p += len;
		*p++ = ' ';
	} while (*++argv);
	p[-1] = '\0';
	return pi->cmdtext;
}

static void insert_bg_job(struct pipe *pi)
{
	struct pipe *thejob;
	int i;

	/* Linear search for the ID of the job to use */
	pi->jobid = 1;
	for (thejob = G.job_list; thejob; thejob = thejob->next)
		if (thejob->jobid >= pi->jobid)
			pi->jobid = thejob->jobid + 1;

	/* Add thejob to the list of running jobs */
	if (!G.job_list) {
		thejob = G.job_list = xmalloc(sizeof(*thejob));
	} else {
		for (thejob = G.job_list; thejob->next; thejob = thejob->next)
			continue;
		thejob->next = xmalloc(sizeof(*thejob));
		thejob = thejob->next;
	}

	/* Physically copy the struct job */
	memcpy(thejob, pi, sizeof(struct pipe));
	thejob->cmds = xzalloc(sizeof(pi->cmds[0]) * pi->num_cmds);
	/* We cannot copy entire pi->cmds[] vector! Double free()s will happen */
	for (i = 0; i < pi->num_cmds; i++) {
// TODO: do we really need to have so many fields which are just dead weight
// at execution stage?
		thejob->cmds[i].pid = pi->cmds[i].pid;
		/* all other fields are not used and stay zero */
	}
	thejob->next = NULL;
	thejob->cmdtext = xstrdup(get_cmdtext(pi));

	/* We don't wait for background thejobs to return -- append it
	   to the list of backgrounded thejobs and leave it alone */
	if (G_interactive_fd)
		printf("[%d] %d %s\n", thejob->jobid, thejob->cmds[0].pid, thejob->cmdtext);
	G.last_bg_pid = thejob->cmds[0].pid;
	G.last_jobid = thejob->jobid;
}

static void remove_bg_job(struct pipe *pi)
{
	struct pipe *prev_pipe;

	if (pi == G.job_list) {
		G.job_list = pi->next;
	} else {
		prev_pipe = G.job_list;
		while (prev_pipe->next != pi)
			prev_pipe = prev_pipe->next;
		prev_pipe->next = pi->next;
	}
	if (G.job_list)
		G.last_jobid = G.job_list->jobid;
	else
		G.last_jobid = 0;
}

/* Remove a backgrounded job */
static void delete_finished_bg_job(struct pipe *pi)
{
	remove_bg_job(pi);
	pi->stopped_cmds = 0;
	free_pipe(pi, 0);
	free(pi);
}
#endif /* JOB */

/* Check to see if any processes have exited -- if they
 * have, figure out why and see if a job has completed */
static int checkjobs(struct pipe* fg_pipe)
{
	int attributes;
	int status;
#if ENABLE_HUSH_JOB
	struct pipe *pi;
#endif
	pid_t childpid;
	int rcode = 0;

	debug_printf_jobs("checkjobs %p\n", fg_pipe);

	errno = 0;
//	if (G.handled_SIGCHLD == G.count_SIGCHLD)
//		/* avoid doing syscall, nothing there anyway */
//		return rcode;

	attributes = WUNTRACED;
	if (fg_pipe == NULL)
		attributes |= WNOHANG;

/* Do we do this right?
 * bash-3.00# sleep 20 | false
 * <ctrl-Z pressed>
 * [3]+  Stopped          sleep 20 | false
 * bash-3.00# echo $?
 * 1   <========== bg pipe is not fully done, but exitcode is already known!
 */

//FIXME: non-interactive bash does not continue even if all processes in fg pipe
//are stopped. Testcase: "cat | cat" in a script (not on command line)
// + killall -STOP cat

 wait_more:
	while (1) {
		int i;
		int dead;

//		i = G.count_SIGCHLD;
		childpid = waitpid(-1, &status, attributes);
		if (childpid <= 0) {
			if (childpid && errno != ECHILD)
				bb_perror_msg("waitpid");
//			else /* Until next SIGCHLD, waitpid's are useless */
//				G.handled_SIGCHLD = i;
			break;
		}
		dead = WIFEXITED(status) || WIFSIGNALED(status);

#if DEBUG_JOBS
		if (WIFSTOPPED(status))
			debug_printf_jobs("pid %d stopped by sig %d (exitcode %d)\n",
					childpid, WSTOPSIG(status), WEXITSTATUS(status));
		if (WIFSIGNALED(status))
			debug_printf_jobs("pid %d killed by sig %d (exitcode %d)\n",
					childpid, WTERMSIG(status), WEXITSTATUS(status));
		if (WIFEXITED(status))
			debug_printf_jobs("pid %d exited, exitcode %d\n",
					childpid, WEXITSTATUS(status));
#endif
		/* Were we asked to wait for fg pipe? */
		if (fg_pipe) {
			for (i = 0; i < fg_pipe->num_cmds; i++) {
				debug_printf_jobs("check pid %d\n", fg_pipe->cmds[i].pid);
				if (fg_pipe->cmds[i].pid != childpid)
					continue;
				/* printf("process %d exit %d\n", i, WEXITSTATUS(status)); */
				if (dead) {
					fg_pipe->cmds[i].pid = 0;
					fg_pipe->alive_cmds--;
					if (i == fg_pipe->num_cmds - 1) {
						/* last process gives overall exitstatus */
						rcode = WEXITSTATUS(status);
						IF_HAS_KEYWORDS(if (fg_pipe->pi_inverted) rcode = !rcode;)
					}
				} else {
					fg_pipe->cmds[i].is_stopped = 1;
					fg_pipe->stopped_cmds++;
				}
				debug_printf_jobs("fg_pipe: alive_cmds %d stopped_cmds %d\n",
						fg_pipe->alive_cmds, fg_pipe->stopped_cmds);
				if (fg_pipe->alive_cmds - fg_pipe->stopped_cmds <= 0) {
					/* All processes in fg pipe have exited/stopped */
#if ENABLE_HUSH_JOB
					if (fg_pipe->alive_cmds)
						insert_bg_job(fg_pipe);
#endif
					return rcode;
				}
				/* There are still running processes in the fg pipe */
				goto wait_more; /* do waitpid again */
			}
			/* it wasnt fg_pipe, look for process in bg pipes */
		}

#if ENABLE_HUSH_JOB
		/* We asked to wait for bg or orphaned children */
		/* No need to remember exitcode in this case */
		for (pi = G.job_list; pi; pi = pi->next) {
			for (i = 0; i < pi->num_cmds; i++) {
				if (pi->cmds[i].pid == childpid)
					goto found_pi_and_prognum;
			}
		}
		/* Happens when shell is used as init process (init=/bin/sh) */
		debug_printf("checkjobs: pid %d was not in our list!\n", childpid);
		continue; /* do waitpid again */

 found_pi_and_prognum:
		if (dead) {
			/* child exited */
			pi->cmds[i].pid = 0;
			pi->alive_cmds--;
			if (!pi->alive_cmds) {
				if (G_interactive_fd)
					printf(JOB_STATUS_FORMAT, pi->jobid,
							"Done", pi->cmdtext);
				delete_finished_bg_job(pi);
			}
		} else {
			/* child stopped */
			pi->cmds[i].is_stopped = 1;
			pi->stopped_cmds++;
		}
#endif
	} /* while (waitpid succeeds)... */

	return rcode;
}

#if ENABLE_HUSH_JOB
static int checkjobs_and_fg_shell(struct pipe* fg_pipe)
{
	pid_t p;
	int rcode = checkjobs(fg_pipe);
	/* Job finished, move the shell to the foreground */
	p = getpgid(0); /* pgid of our process */
	debug_printf_jobs("fg'ing ourself: getpgid(0)=%d\n", (int)p);
	tcsetpgrp(G_interactive_fd, p);
	return rcode;
}
#endif

/* Start all the jobs, but don't wait for anything to finish.
 * See checkjobs().
 *
 * Return code is normally -1, when the caller has to wait for children
 * to finish to determine the exit status of the pipe.  If the pipe
 * is a simple builtin command, however, the action is done by the
 * time run_pipe returns, and the exit code is provided as the
 * return value.
 *
 * Returns -1 only if started some children. IOW: we have to
 * mask out retvals of builtins etc with 0xff!
 *
 * The only case when we do not need to [v]fork is when the pipe
 * is single, non-backgrounded, non-subshell command. Examples:
 * cmd ; ...   { list } ; ...
 * cmd && ...  { list } && ...
 * cmd || ...  { list } || ...
 * If it is, then we can run cmd as a builtin, NOFORK [do we do this?],
 * or (if SH_STANDALONE) an applet, and we can run the { list }
 * with run_list(). If it isn't one of these, we fork and exec cmd.
 *
 * Cases when we must fork:
 * non-single:   cmd | cmd
 * backgrounded: cmd &     { list } &
 * subshell:     ( list ) [&]
 */
static int run_pipe(struct pipe *pi)
{
	static const char *const null_ptr = NULL;
	int i;
	int nextin;
	int pipefds[2];		/* pipefds[0] is for reading */
	struct command *command;
	char **argv_expanded;
	char **argv;
	char *p;
	/* it is not always needed, but we aim to smaller code */
	int squirrel[] = { -1, -1, -1 };
	int rcode;

	debug_printf_exec("run_pipe start: members:%d\n", pi->num_cmds);

	USE_HUSH_JOB(pi->pgrp = -1;)
	pi->stopped_cmds = 0;
	command = &(pi->cmds[0]);
	argv_expanded = NULL;

	if (pi->num_cmds != 1
	 || pi->followup == PIPE_BG
	 || command->grp_type == GRP_SUBSHELL
	) {
		goto must_fork;
	}

	pi->alive_cmds = 1;

	debug_printf_exec(": group:%p argv:'%s'\n",
		command->group, command->argv ? command->argv[0] : "NONE");

	if (command->group) {
#if ENABLE_HUSH_FUNCTIONS
		if (command->grp_type == GRP_FUNCTION) {
			/* func () { list } */
			bb_error_msg("here we ought to remember function definition, and go on");
			return EXIT_SUCCESS;
		}
#endif
		/* { list } */
		debug_printf("non-subshell group\n");
		setup_redirects(command, squirrel);
		debug_printf_exec(": run_list\n");
		rcode = run_list(command->group) & 0xff;
		restore_redirects(squirrel);
		debug_printf_exec("run_pipe return %d\n", rcode);
		IF_HAS_KEYWORDS(if (pi->pi_inverted) rcode = !rcode;)
		return rcode;
	}

	argv = command->argv ? command->argv : (char **) &null_ptr;
	{
		const struct built_in_command *x;
		char **new_env = NULL;
		char **old_env = NULL;

		if (argv[command->assignment_cnt] == NULL) {
			/* Assignments, but no command */
			/* Ensure redirects take effect. Try "a=t >file" */
			setup_redirects(command, squirrel);
			restore_redirects(squirrel);
			/* Set shell variables */
			while (*argv) {
				p = expand_string_to_string(*argv);
				debug_printf_exec("set shell var:'%s'->'%s'\n",
						*argv, p);
				set_local_var(p, 0, 0);
				argv++;
			}
			/* Do we need to flag set_local_var() errors?
			 * "assignment to readonly var" and "putenv error"
			 */
			return EXIT_SUCCESS;
		}

		/* Expand the rest into (possibly) many strings each */
		argv_expanded = expand_strvec_to_strvec(argv + command->assignment_cnt);

		for (x = bltins; x != &bltins[ARRAY_SIZE(bltins)]; x++) {
			if (strcmp(argv_expanded[0], x->cmd) != 0)
				continue;
			if (x->function == builtin_exec && argv_expanded[1] == NULL) {
				debug_printf("exec with redirects only\n");
				setup_redirects(command, NULL);
				rcode = EXIT_SUCCESS;
				goto clean_up_and_ret1;
			}
			debug_printf("builtin inline %s\n", argv_expanded[0]);
			/* XXX setup_redirects acts on file descriptors, not FILEs.
			 * This is perfect for work that comes after exec().
			 * Is it really safe for inline use?  Experimentally,
			 * things seem to work with glibc. */
			setup_redirects(command, squirrel);
			new_env = expand_assignments(argv, command->assignment_cnt);
			old_env = putenv_all_and_save_old(new_env);
			debug_printf_exec(": builtin '%s' '%s'...\n",
				    x->cmd, argv_expanded[1]);
			rcode = x->function(argv_expanded) & 0xff;
#if ENABLE_FEATURE_SH_STANDALONE
 clean_up_and_ret:
#endif
			restore_redirects(squirrel);
			free_strings_and_unsetenv(new_env, 1);
			putenv_all(old_env);
			/* Free the pointers, but the strings themselves
			 * are in environ now, don't use free_strings! */
			free(old_env);
 clean_up_and_ret1:
			free(argv_expanded);
			IF_HAS_KEYWORDS(if (pi->pi_inverted) rcode = !rcode;)
			debug_printf_exec("run_pipe return %d\n", rcode);
			return rcode;
		}
#if ENABLE_FEATURE_SH_STANDALONE
		i = find_applet_by_name(argv_expanded[0]);
		if (i >= 0 && APPLET_IS_NOFORK(i)) {
			setup_redirects(command, squirrel);
			save_nofork_data(&G.nofork_save);
			new_env = expand_assignments(argv, command->assignment_cnt);
			old_env = putenv_all_and_save_old(new_env);
			debug_printf_exec(": run_nofork_applet '%s' '%s'...\n",
					argv_expanded[0], argv_expanded[1]);
			rcode = run_nofork_applet_prime(&G.nofork_save, i, argv_expanded);
			goto clean_up_and_ret;
		}
#endif
		/* It is neither builtin nor applet. We must fork. */
	}

 must_fork:
	/* NB: argv_expanded may already be created, and that
	 * might include `cmd` runs! Do not rerun it! We *must*
	 * use argv_expanded if it's non-NULL */

	/* Going to fork a child per each pipe member */
	pi->alive_cmds = 0;
	nextin = 0;

	for (i = 0; i < pi->num_cmds; i++) {
#if !BB_MMU
		volatile nommu_save_t nommu_save;
		nommu_save.new_env = NULL;
		nommu_save.old_env = NULL;
		nommu_save.argv = NULL;
#endif
		command = &(pi->cmds[i]);
		if (command->argv) {
			debug_printf_exec(": pipe member '%s' '%s'...\n",
					command->argv[0], command->argv[1]);
		} else {
			debug_printf_exec(": pipe member with no argv\n");
		}

		/* pipes are inserted between pairs of commands */
		pipefds[0] = 0;
		pipefds[1] = 1;
		if ((i + 1) < pi->num_cmds)
			xpipe(pipefds);

		command->pid = BB_MMU ? fork() : vfork();
		if (!command->pid) { /* child */
#if ENABLE_HUSH_JOB
			disable_restore_tty_pgrp_on_exit();

			/* Every child adds itself to new process group
			 * with pgid == pid_of_first_child_in_pipe */
			if (G.run_list_level == 1 && G_interactive_fd) {
				pid_t pgrp;
				pgrp = pi->pgrp;
				if (pgrp < 0) /* true for 1st process only */
					pgrp = getpid();
				if (setpgid(0, pgrp) == 0 && pi->followup != PIPE_BG) {
					/* We do it in *every* child, not just first,
					 * to avoid races */
					tcsetpgrp(G_interactive_fd, pgrp);
				}
			}
#endif
			xmove_fd(nextin, 0);
			xmove_fd(pipefds[1], 1); /* write end */
			if (pipefds[0] > 1)
				close(pipefds[0]); /* read end */
			/* Like bash, explicit redirects override pipes,
			 * and the pipe fd is available for dup'ing. */
			setup_redirects(command, NULL);

			/* Restore default handlers just prior to exec */
			/*signal(SIGCHLD, SIG_DFL); - so far we don't have any handlers */

			/* Stores to nommu_save list of env vars putenv'ed
			 * (NOMMU, on MMU we don't need that) */
			/* cast away volatility... */
			pseudo_exec((nommu_save_t*) &nommu_save, command, argv_expanded);
			/* pseudo_exec() does not return */
		}

		/* parent or error */
		enable_restore_tty_pgrp_on_exit();
#if !BB_MMU
		/* Clean up after vforked child */
		clean_up_after_re_execute();
		free(nommu_save.argv);
		free_strings_and_unsetenv(nommu_save.new_env, 1);
		putenv_all(nommu_save.old_env);
		/* Free the pointers, but the strings themselves
		 * are in environ now, don't use free_strings! */
		free(nommu_save.old_env);
#endif
		free(argv_expanded);
		argv_expanded = NULL;
		if (command->pid < 0) { /* [v]fork failed */
			/* Clearly indicate, was it fork or vfork */
			bb_perror_msg(BB_MMU ? "fork" : "vfork");
		} else {
			pi->alive_cmds++;
#if ENABLE_HUSH_JOB
			/* Second and next children need to know pid of first one */
			if (pi->pgrp < 0)
				pi->pgrp = command->pid;
#endif
		}

		if (i)
			close(nextin);
		if ((i + 1) < pi->num_cmds)
			close(pipefds[1]); /* write end */
		/* Pass read (output) pipe end to next iteration */
		nextin = pipefds[0];
	}

	if (!pi->alive_cmds) {
		debug_printf_exec("run_pipe return 1 (all forks failed, no children)\n");
		return 1;
	}

	debug_printf_exec("run_pipe return -1 (%u children started)\n", pi->alive_cmds);
	return -1;
}

#ifndef debug_print_tree
static void debug_print_tree(struct pipe *pi, int lvl)
{
	static const char *const PIPE[] = {
		[PIPE_SEQ] = "SEQ",
		[PIPE_AND] = "AND",
		[PIPE_OR ] = "OR" ,
		[PIPE_BG ] = "BG" ,
	};
	static const char *RES[] = {
		[RES_NONE ] = "NONE" ,
#if ENABLE_HUSH_IF
		[RES_IF   ] = "IF"   ,
		[RES_THEN ] = "THEN" ,
		[RES_ELIF ] = "ELIF" ,
		[RES_ELSE ] = "ELSE" ,
		[RES_FI   ] = "FI"   ,
#endif
#if ENABLE_HUSH_LOOPS
		[RES_FOR  ] = "FOR"  ,
		[RES_WHILE] = "WHILE",
		[RES_UNTIL] = "UNTIL",
		[RES_DO   ] = "DO"   ,
		[RES_DONE ] = "DONE" ,
#endif
#if ENABLE_HUSH_LOOPS || ENABLE_HUSH_CASE
		[RES_IN   ] = "IN"   ,
#endif
#if ENABLE_HUSH_CASE
		[RES_CASE ] = "CASE" ,
		[RES_MATCH] = "MATCH",
		[RES_CASEI] = "CASEI",
		[RES_ESAC ] = "ESAC" ,
#endif
		[RES_XXXX ] = "XXXX" ,
		[RES_SNTX ] = "SNTX" ,
	};
	static const char *const GRPTYPE[] = {
		"{}",
		"()",
#if ENABLE_HUSH_FUNCTIONS
		"func()",
#endif
	};

	int pin, prn;

	pin = 0;
	while (pi) {
		fprintf(stderr, "%*spipe %d res_word=%s followup=%d %s\n", lvl*2, "",
				pin, RES[pi->res_word], pi->followup, PIPE[pi->followup]);
		prn = 0;
		while (prn < pi->num_cmds) {
			struct command *command = &pi->cmds[prn];
			char **argv = command->argv;

			fprintf(stderr, "%*s cmd %d assignment_cnt:%d",
					lvl*2, "", prn,
					command->assignment_cnt);
			if (command->group) {
				fprintf(stderr, " group %s: (argv=%p)\n",
						GRPTYPE[command->grp_type],
						argv);
				debug_print_tree(command->group, lvl+1);
				prn++;
				continue;
			}
			if (argv) while (*argv) {
				fprintf(stderr, " '%s'", *argv);
				argv++;
			}
			fprintf(stderr, "\n");
			prn++;
		}
		pi = pi->next;
		pin++;
	}
}
#endif

/* NB: called by pseudo_exec, and therefore must not modify any
 * global data until exec/_exit (we can be a child after vfork!) */
static int run_list(struct pipe *pi)
{
#if ENABLE_HUSH_CASE
	char *case_word = NULL;
#endif
#if ENABLE_HUSH_LOOPS
	struct pipe *loop_top = NULL;
	char *for_varname = NULL;
	char **for_lcur = NULL;
	char **for_list = NULL;
#endif
	smallint last_followup;
	smalluint rcode;
#if ENABLE_HUSH_IF || ENABLE_HUSH_CASE
	smalluint cond_code = 0;
#else
	enum { cond_code = 0 };
#endif
#if HAS_KEYWORDS
	smallint rword; /* enum reserved_style */
	smallint last_rword; /* ditto */
#endif

	debug_printf_exec("run_list start lvl %d\n", G.run_list_level + 1);

#if ENABLE_HUSH_LOOPS
	/* Check syntax for "for" */
	for (struct pipe *cpipe = pi; cpipe; cpipe = cpipe->next) {
		if (cpipe->res_word != RES_FOR && cpipe->res_word != RES_IN)
			continue;
		/* current word is FOR or IN (BOLD in comments below) */
		if (cpipe->next == NULL) {
			syntax("malformed for");
			debug_printf_exec("run_list lvl %d return 1\n", G.run_list_level);
			return 1;
		}
		/* "FOR v; do ..." and "for v IN a b; do..." are ok */
		if (cpipe->next->res_word == RES_DO)
			continue;
		/* next word is not "do". It must be "in" then ("FOR v in ...") */
		if (cpipe->res_word == RES_IN /* "for v IN a b; not_do..."? */
		 || cpipe->next->res_word != RES_IN /* FOR v not_do_and_not_in..."? */
		) {
			syntax("malformed for");
			debug_printf_exec("run_list lvl %d return 1\n", G.run_list_level);
			return 1;
		}
	}
#endif

	/* Past this point, all code paths should jump to ret: label
	 * in order to return, no direct "return" statements please.
	 * This helps to ensure that no memory is leaked. */

////TODO: ctrl-Z handling needs re-thinking and re-testing

#if ENABLE_HUSH_JOB
	/* Example of nested list: "while true; do { sleep 1 | exit 2; } done".
	 * We are saving state before entering outermost list ("while...done")
	 * so that ctrl-Z will correctly background _entire_ outermost list,
	 * not just a part of it (like "sleep 1 | exit 2") */
	if (++G.run_list_level == 1 && G_interactive_fd) {
		if (sigsetjmp(G.toplevel_jb, 1)) {
			/* ctrl-Z forked and we are parent; or ctrl-C.
			 * Sighandler has longjmped us here */
			signal(SIGINT, SIG_IGN);
			signal(SIGTSTP, SIG_IGN);
			/* Restore level (we can be coming from deep inside
			 * nested levels) */
			G.run_list_level = 1;
#if ENABLE_FEATURE_SH_STANDALONE
			if (G.nofork_save.saved) { /* if save area is valid */
				debug_printf_jobs("exiting nofork early\n");
				restore_nofork_data(&G.nofork_save);
			}
#endif
////			if (G.ctrl_z_flag) {
////				/* ctrl-Z has forked and stored pid of the child in pi->pid.
////				 * Remember this child as background job */
////				insert_bg_job(pi);
////			} else {
				/* ctrl-C. We just stop doing whatever we were doing */
				bb_putchar('\n');
////			}
			USE_HUSH_LOOPS(loop_top = NULL;)
			USE_HUSH_LOOPS(G.depth_of_loop = 0;)
			rcode = 0;
			goto ret;
		}
////		/* ctrl-Z handler will store pid etc in pi */
////		G.toplevel_list = pi;
////		G.ctrl_z_flag = 0;
////#if ENABLE_FEATURE_SH_STANDALONE
////		G.nofork_save.saved = 0; /* in case we will run a nofork later */
////#endif
////		signal_SA_RESTART_empty_mask(SIGTSTP, handler_ctrl_z);
////		signal(SIGINT, handler_ctrl_c);
	}
#endif /* JOB */

#if HAS_KEYWORDS
	rword = RES_NONE;
	last_rword = RES_XXXX;
#endif
	last_followup = PIPE_SEQ;
	rcode = G.last_exitcode;

	/* Go through list of pipes, (maybe) executing them. */
	for (; pi; pi = USE_HUSH_LOOPS(rword == RES_DONE ? loop_top : ) pi->next) {
		if (G.flag_SIGINT)
			break;

		IF_HAS_KEYWORDS(rword = pi->res_word;)
		debug_printf_exec(": rword=%d cond_code=%d last_rword=%d\n",
				rword, cond_code, last_rword);
#if ENABLE_HUSH_LOOPS
		if ((rword == RES_WHILE || rword == RES_UNTIL || rword == RES_FOR)
		 && loop_top == NULL /* avoid bumping G.depth_of_loop twice */
		) {
			/* start of a loop: remember where loop starts */
			loop_top = pi;
			G.depth_of_loop++;
		}
#endif
		/* Still in the same "if...", "then..." or "do..." branch? */
		if (IF_HAS_KEYWORDS(rword == last_rword &&) 1) {
			if ((rcode == 0 && last_followup == PIPE_OR)
			 || (rcode != 0 && last_followup == PIPE_AND)
			) {
				/* It is "<true> || CMD" or "<false> && CMD"
				 * and we should not execute CMD */
				debug_printf_exec("skipped cmd because of || or &&\n");
				last_followup = pi->followup;
				continue;
			}
		}
		last_followup = pi->followup;
		IF_HAS_KEYWORDS(last_rword = rword;)
#if ENABLE_HUSH_IF
		if (cond_code) {
			if (rword == RES_THEN) {
				/* if false; then ... fi has exitcode 0! */
				G.last_exitcode = rcode = EXIT_SUCCESS;
				/* "if <false> THEN cmd": skip cmd */
				continue;
			}
		} else {
			if (rword == RES_ELSE || rword == RES_ELIF) {
				/* "if <true> then ... ELSE/ELIF cmd":
				 * skip cmd and all following ones */
				break;
			}
		}
#endif
#if ENABLE_HUSH_LOOPS
		if (rword == RES_FOR) { /* && pi->num_cmds - always == 1 */
			if (!for_lcur) {
				/* first loop through for */

				static const char encoded_dollar_at[] ALIGN1 = {
					SPECIAL_VAR_SYMBOL, '@' | 0x80, SPECIAL_VAR_SYMBOL, '\0'
				}; /* encoded representation of "$@" */
				static const char *const encoded_dollar_at_argv[] = {
					encoded_dollar_at, NULL
				}; /* argv list with one element: "$@" */
				char **vals;

				vals = (char**)encoded_dollar_at_argv;
				if (pi->next->res_word == RES_IN) {
					/* if no variable values after "in" we skip "for" */
					if (!pi->next->cmds[0].argv) {
						G.last_exitcode = rcode = EXIT_SUCCESS;
						debug_printf_exec(": null FOR: exitcode EXIT_SUCCESS\n");
						break;
					}
					vals = pi->next->cmds[0].argv;
				} /* else: "for var; do..." -> assume "$@" list */
				/* create list of variable values */
				debug_print_strings("for_list made from", vals);
				for_list = expand_strvec_to_strvec(vals);
				for_lcur = for_list;
				debug_print_strings("for_list", for_list);
				for_varname = pi->cmds[0].argv[0];
				pi->cmds[0].argv[0] = NULL;
			}
			free(pi->cmds[0].argv[0]);
			if (!*for_lcur) {
				/* "for" loop is over, clean up */
				free(for_list);
				for_list = NULL;
				for_lcur = NULL;
				pi->cmds[0].argv[0] = for_varname;
				break;
			}
			/* Insert next value from for_lcur */
//TODO: does it need escaping?
			pi->cmds[0].argv[0] = xasprintf("%s=%s", for_varname, *for_lcur++);
			pi->cmds[0].assignment_cnt = 1;
		}
		if (rword == RES_IN) {
			continue; /* "for v IN list;..." - "in" has no cmds anyway */
		}
		if (rword == RES_DONE) {
			continue; /* "done" has no cmds too */
		}
#endif
#if ENABLE_HUSH_CASE
		if (rword == RES_CASE) {
			case_word = expand_strvec_to_string(pi->cmds->argv);
			continue;
		}
		if (rword == RES_MATCH) {
			char **argv;

			if (!case_word) /* "case ... matched_word) ... WORD)": we executed selected branch, stop */
				break;
			/* all prev words didn't match, does this one match? */
			argv = pi->cmds->argv;
			while (*argv) {
				char *pattern = expand_string_to_string(*argv);
				/* TODO: which FNM_xxx flags to use? */
				cond_code = (fnmatch(pattern, case_word, /*flags:*/ 0) != 0);
				free(pattern);
				if (cond_code == 0) { /* match! we will execute this branch */
					free(case_word); /* make future "word)" stop */
					case_word = NULL;
					break;
				}
				argv++;
			}
			continue;
		}
		if (rword == RES_CASEI) { /* inside of a case branch */
			if (cond_code != 0)
				continue; /* not matched yet, skip this pipe */
		}
#endif
		/* Just pressing <enter> in shell should check for jobs.
		 * OTOH, in non-interactive shell this is useless
		 * and only leads to extra job checks */
		if (pi->num_cmds == 0) {
			if (G_interactive_fd)
				goto check_jobs_and_continue;
			continue;
		}

		/* After analyzing all keywords and conditions, we decided
		 * to execute this pipe. NB: have to do checkjobs(NULL)
		 * after run_pipe to collect any background children,
		 * even if list execution is to be stopped. */
		debug_printf_exec(": run_pipe with %d members\n", pi->num_cmds);
		{
			int r;
#if ENABLE_HUSH_LOOPS
			G.flag_break_continue = 0;
#endif
			rcode = r = run_pipe(pi); /* NB: rcode is a smallint */
			if (r != -1) {
				/* We only ran a builtin: rcode is already known
				 * and we don't need to wait for anything. */
				G.last_exitcode = rcode;
				debug_printf_exec(": builtin exitcode %d\n", rcode);
				check_and_run_traps(0);
#if ENABLE_HUSH_LOOPS
				/* Was it "break" or "continue"? */
				if (G.flag_break_continue) {
					smallint fbc = G.flag_break_continue;
					/* We might fall into outer *loop*,
					 * don't want to break it too */
					if (loop_top) {
						G.depth_break_continue--;
						if (G.depth_break_continue == 0)
							G.flag_break_continue = 0;
						/* else: e.g. "continue 2" should *break* once, *then* continue */
					} /* else: "while... do... { we are here (innermost list is not a loop!) };...done" */
					if (G.depth_break_continue != 0 || fbc == BC_BREAK)
						goto check_jobs_and_break;
					/* "continue": simulate end of loop */
					rword = RES_DONE;
					continue;
				}
#endif
			} else if (pi->followup == PIPE_BG) {
				/* What does bash do with attempts to background builtins? */
				/* even bash 3.2 doesn't do that well with nested bg:
				 * try "{ { sleep 10; echo DEEP; } & echo HERE; } &".
				 * I'm NOT treating inner &'s as jobs */
				check_and_run_traps(0);
#if ENABLE_HUSH_JOB
				if (G.run_list_level == 1)
					insert_bg_job(pi);
#endif
				G.last_exitcode = rcode = EXIT_SUCCESS;
				debug_printf_exec(": cmd&: exitcode EXIT_SUCCESS\n");
			} else {
#if ENABLE_HUSH_JOB
				if (G.run_list_level == 1 && G_interactive_fd) {
					/* Waits for completion, then fg's main shell */
					rcode = checkjobs_and_fg_shell(pi);
					debug_printf_exec(": checkjobs_and_fg_shell exitcode %d\n", rcode);
					check_and_run_traps(0);
				} else
#endif
				{ /* This one just waits for completion */
					rcode = checkjobs(pi);
					debug_printf_exec(": checkjobs exitcode %d\n", rcode);
					check_and_run_traps(0);
				}
				G.last_exitcode = rcode;
			}
		}

		/* Analyze how result affects subsequent commands */
#if ENABLE_HUSH_IF
		if (rword == RES_IF || rword == RES_ELIF)
			cond_code = rcode;
#endif
#if ENABLE_HUSH_LOOPS
		/* Beware of "while false; true; do ..."! */
		if (pi->next && pi->next->res_word == RES_DO) {
			if (rword == RES_WHILE) {
				if (rcode) {
					/* "while false; do...done" - exitcode 0 */
					G.last_exitcode = rcode = EXIT_SUCCESS;
					debug_printf_exec(": while expr is false: breaking (exitcode:EXIT_SUCCESS)\n");
					goto check_jobs_and_break;
				}
			}
			if (rword == RES_UNTIL) {
				if (!rcode) {
					debug_printf_exec(": until expr is true: breaking\n");
 check_jobs_and_break:
					checkjobs(NULL);
					break;
				}
			}
		}
#endif

 check_jobs_and_continue:
		checkjobs(NULL);
	} /* for (pi) */

#if ENABLE_HUSH_JOB
////	if (G.ctrl_z_flag) {
////		/* ctrl-Z forked somewhere in the past, we are the child,
////		 * and now we completed running the list. Exit. */
//////TODO: _exit?
////		exit(rcode);
////	}
 ret:
	G.run_list_level--;
////	if (!G.run_list_level && G_interactive_fd) {
////		signal(SIGTSTP, SIG_IGN);
////		signal(SIGINT, SIG_IGN);
////	}
#endif
	debug_printf_exec("run_list lvl %d return %d\n", G.run_list_level + 1, rcode);
#if ENABLE_HUSH_LOOPS
	if (loop_top)
		G.depth_of_loop--;
	free(for_list);
#endif
#if ENABLE_HUSH_CASE
	free(case_word);
#endif
	return rcode;
}

/* Select which version we will use */
static int run_and_free_list(struct pipe *pi)
{
	int rcode = 0;
	debug_printf_exec("run_and_free_list entered\n");
	if (!G.fake_mode) {
		debug_printf_exec(": run_list: 1st pipe with %d cmds\n", pi->num_cmds);
		rcode = run_list(pi);
	}
	/* free_pipe_list has the side effect of clearing memory.
	 * In the long run that function can be merged with run_list,
	 * but doing that now would hobble the debugging effort. */
	free_pipe_list(pi, /* indent: */ 0);
	debug_printf_exec("run_and_free_list return %d\n", rcode);
	return rcode;
}


static struct pipe *new_pipe(void)
{
	struct pipe *pi;
	pi = xzalloc(sizeof(struct pipe));
	/*pi->followup = 0; - deliberately invalid value */
	/*pi->res_word = RES_NONE; - RES_NONE is 0 anyway */
	return pi;
}

/* Command (member of a pipe) is complete. The only possible error here
 * is out of memory, in which case xmalloc exits. */
static int done_command(struct parse_context *ctx)
{
	/* The command is really already in the pipe structure, so
	 * advance the pipe counter and make a new, null command. */
	struct pipe *pi = ctx->pipe;
	struct command *command = ctx->command;

	if (command) {
		if (command->group == NULL
		 && command->argv == NULL
		 && command->redirects == NULL
		) {
			debug_printf_parse("done_command: skipping null cmd, num_cmds=%d\n", pi->num_cmds);
			memset(command, 0, sizeof(*command)); /* paranoia */
			return pi->num_cmds;
		}
		pi->num_cmds++;
		debug_printf_parse("done_command: ++num_cmds=%d\n", pi->num_cmds);
		//debug_print_tree(ctx->list_head, 20);
	} else {
		debug_printf_parse("done_command: initializing, num_cmds=%d\n", pi->num_cmds);
	}

	/* Only real trickiness here is that the uncommitted
	 * command structure is not counted in pi->num_cmds. */
	pi->cmds = xrealloc(pi->cmds, sizeof(*pi->cmds) * (pi->num_cmds+1));
	command = &pi->cmds[pi->num_cmds];
	memset(command, 0, sizeof(*command));

	ctx->command = command;
	/* but ctx->pipe and ctx->list_head remain unchanged */

	return pi->num_cmds; /* used only for 0/nonzero check */
}

static void done_pipe(struct parse_context *ctx, pipe_style type)
{
	int not_null;

	debug_printf_parse("done_pipe entered, followup %d\n", type);
	/* Close previous command */
	not_null = done_command(ctx);
	ctx->pipe->followup = type;
#if HAS_KEYWORDS
	ctx->pipe->pi_inverted = ctx->ctx_inverted;
	ctx->ctx_inverted = 0;
	ctx->pipe->res_word = ctx->ctx_res_w;
#endif

	/* Without this check, even just <enter> on command line generates
	 * tree of three NOPs (!). Which is harmless but annoying.
	 * IOW: it is safe to do it unconditionally.
	 * RES_NONE case is for "for a in; do ..." (empty IN set)
	 * and other cases to work. */
	if (not_null
#if HAS_KEYWORDS
	 || ctx->ctx_res_w == RES_FI
	 || ctx->ctx_res_w == RES_DONE
	 || ctx->ctx_res_w == RES_FOR
	 || ctx->ctx_res_w == RES_IN
	 || ctx->ctx_res_w == RES_ESAC
#endif
	) {
		struct pipe *new_p;
		debug_printf_parse("done_pipe: adding new pipe: "
				"not_null:%d ctx->ctx_res_w:%d\n",
				not_null, ctx->ctx_res_w);
		new_p = new_pipe();
		ctx->pipe->next = new_p;
		ctx->pipe = new_p;
		/* RES_THEN, RES_DO etc are "sticky" -
		 * they remain set for commands inside if/while.
		 * This is used to control execution.
		 * RES_FOR and RES_IN are NOT sticky (needed to support
		 * cases where variable or value happens to match a keyword):
		 */
#if ENABLE_HUSH_LOOPS
		if (ctx->ctx_res_w == RES_FOR
		 || ctx->ctx_res_w == RES_IN)
			ctx->ctx_res_w = RES_NONE;
#endif
#if ENABLE_HUSH_CASE
		if (ctx->ctx_res_w == RES_MATCH)
			ctx->ctx_res_w = RES_CASEI;
#endif
		ctx->command = NULL; /* trick done_command below */
		/* Create the memory for command, roughly:
		 * ctx->pipe->cmds = new struct command;
		 * ctx->command = &ctx->pipe->cmds[0];
		 */
		done_command(ctx);
		//debug_print_tree(ctx->list_head, 10);
	}
	debug_printf_parse("done_pipe return\n");
}

static void initialize_context(struct parse_context *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->pipe = ctx->list_head = new_pipe();
	/* Create the memory for command, roughly:
	 * ctx->pipe->cmds = new struct command;
	 * ctx->command = &ctx->pipe->cmds[0];
	 */
	done_command(ctx);
}

/* If a reserved word is found and processed, parse context is modified
 * and 1 is returned.
 */
#if HAS_KEYWORDS
struct reserved_combo {
	char literal[6];
	unsigned char res;
	unsigned char assignment_flag;
	int flag;
};
enum {
	FLAG_END   = (1 << RES_NONE ),
#if ENABLE_HUSH_IF
	FLAG_IF    = (1 << RES_IF   ),
	FLAG_THEN  = (1 << RES_THEN ),
	FLAG_ELIF  = (1 << RES_ELIF ),
	FLAG_ELSE  = (1 << RES_ELSE ),
	FLAG_FI    = (1 << RES_FI   ),
#endif
#if ENABLE_HUSH_LOOPS
	FLAG_FOR   = (1 << RES_FOR  ),
	FLAG_WHILE = (1 << RES_WHILE),
	FLAG_UNTIL = (1 << RES_UNTIL),
	FLAG_DO    = (1 << RES_DO   ),
	FLAG_DONE  = (1 << RES_DONE ),
	FLAG_IN    = (1 << RES_IN   ),
#endif
#if ENABLE_HUSH_CASE
	FLAG_MATCH = (1 << RES_MATCH),
	FLAG_ESAC  = (1 << RES_ESAC ),
#endif
	FLAG_START = (1 << RES_XXXX ),
};

static const struct reserved_combo* match_reserved_word(o_string *word)
{
	/* Mostly a list of accepted follow-up reserved words.
	 * FLAG_END means we are done with the sequence, and are ready
	 * to turn the compound list into a command.
	 * FLAG_START means the word must start a new compound list.
	 */
	static const struct reserved_combo reserved_list[] = {
#if ENABLE_HUSH_IF
		{ "!",     RES_NONE,  NOT_ASSIGNMENT , 0 },
		{ "if",    RES_IF,    WORD_IS_KEYWORD, FLAG_THEN | FLAG_START },
		{ "then",  RES_THEN,  WORD_IS_KEYWORD, FLAG_ELIF | FLAG_ELSE | FLAG_FI },
		{ "elif",  RES_ELIF,  WORD_IS_KEYWORD, FLAG_THEN },
		{ "else",  RES_ELSE,  WORD_IS_KEYWORD, FLAG_FI   },
		{ "fi",    RES_FI,    NOT_ASSIGNMENT , FLAG_END  },
#endif
#if ENABLE_HUSH_LOOPS
		{ "for",   RES_FOR,   NOT_ASSIGNMENT , FLAG_IN | FLAG_DO | FLAG_START },
		{ "while", RES_WHILE, WORD_IS_KEYWORD, FLAG_DO | FLAG_START },
		{ "until", RES_UNTIL, WORD_IS_KEYWORD, FLAG_DO | FLAG_START },
		{ "in",    RES_IN,    NOT_ASSIGNMENT , FLAG_DO   },
		{ "do",    RES_DO,    WORD_IS_KEYWORD, FLAG_DONE },
		{ "done",  RES_DONE,  NOT_ASSIGNMENT , FLAG_END  },
#endif
#if ENABLE_HUSH_CASE
		{ "case",  RES_CASE,  NOT_ASSIGNMENT , FLAG_MATCH | FLAG_START },
		{ "esac",  RES_ESAC,  NOT_ASSIGNMENT , FLAG_END  },
#endif
	};
	const struct reserved_combo *r;

	for (r = reserved_list;	r < reserved_list + ARRAY_SIZE(reserved_list); r++) {
		if (strcmp(word->data, r->literal) == 0)
			return r;
	}
	return NULL;
}
static int reserved_word(o_string *word, struct parse_context *ctx)
{
#if ENABLE_HUSH_CASE
	static const struct reserved_combo reserved_match = {
		"",        RES_MATCH, NOT_ASSIGNMENT , FLAG_MATCH | FLAG_ESAC
	};
#endif
	const struct reserved_combo *r;

	r = match_reserved_word(word);
	if (!r)
		return 0;

	debug_printf("found reserved word %s, res %d\n", r->literal, r->res);
#if ENABLE_HUSH_CASE
	if (r->res == RES_IN && ctx->ctx_res_w == RES_CASE)
		/* "case word IN ..." - IN part starts first match part */
		r = &reserved_match;
	else
#endif
	if (r->flag == 0) { /* '!' */
		if (ctx->ctx_inverted) { /* bash doesn't accept '! ! true' */
			syntax("! ! command");
			IF_HAS_KEYWORDS(ctx->ctx_res_w = RES_SNTX;)
		}
		ctx->ctx_inverted = 1;
		return 1;
	}
	if (r->flag & FLAG_START) {
		struct parse_context *old;
		old = xmalloc(sizeof(*old));
		debug_printf_parse("push stack %p\n", old);
		*old = *ctx;   /* physical copy */
		initialize_context(ctx);
		ctx->stack = old;
	} else if (/*ctx->ctx_res_w == RES_NONE ||*/ !(ctx->old_flag & (1 << r->res))) {
		syntax(word->data);
		ctx->ctx_res_w = RES_SNTX;
		return 1;
	}
	ctx->ctx_res_w = r->res;
	ctx->old_flag = r->flag;
	if (ctx->old_flag & FLAG_END) {
		struct parse_context *old;
		done_pipe(ctx, PIPE_SEQ);
		debug_printf_parse("pop stack %p\n", ctx->stack);
		old = ctx->stack;
		old->command->group = ctx->list_head;
		old->command->grp_type = GRP_NORMAL;
#if !BB_MMU
		o_addstr(&old->as_string, ctx->as_string.data);
		o_free_unsafe(&ctx->as_string);
		old->command->group_as_string = xstrdup(old->as_string.data);
		debug_printf_parse("pop, remembering as:'%s'\n",
				old->command->group_as_string);
#endif
		*ctx = *old;   /* physical copy */
		free(old);
	}
	word->o_assignment = r->assignment_flag;
	return 1;
}
#endif

/* Word is complete, look at it and update parsing context.
 * Normal return is 0. Syntax errors return 1.
 * Note: on return, word is reset, but not o_free'd!
 */
static int done_word(o_string *word, struct parse_context *ctx)
{
	struct command *command = ctx->command;

	debug_printf_parse("done_word entered: '%s' %p\n", word->data, command);
	if (word->length == 0 && word->o_quoted == 0) {
		debug_printf_parse("done_word return 0: true null, ignored\n");
		return 0;
	}
	/* If this word wasn't an assignment, next ones definitely
	 * can't be assignments. Even if they look like ones. */
	if (word->o_assignment != DEFINITELY_ASSIGNMENT
	 && word->o_assignment != WORD_IS_KEYWORD
	) {
		word->o_assignment = NOT_ASSIGNMENT;
	} else {
		if (word->o_assignment == DEFINITELY_ASSIGNMENT)
			command->assignment_cnt++;
		word->o_assignment = MAYBE_ASSIGNMENT;
	}

	if (ctx->pending_redirect) {
		/* We do not glob in e.g. >*.tmp case. bash seems to glob here
		 * only if run as "bash", not "sh" */
		/* http://www.opengroup.org/onlinepubs/009695399/utilities/xcu_chap02.html
		 * "2.7 Redirection
		 * ...the word that follows the redirection operator
		 * shall be subjected to tilde expansion, parameter expansion,
		 * command substitution, arithmetic expansion, and quote
		 * removal. Pathname expansion shall not be performed
		 * on the word by a non-interactive shell; an interactive
		 * shell may perform it, but shall do so only when
		 * the expansion would result in one word."
		 */
		ctx->pending_redirect->rd_filename = xstrdup(word->data);
		if (ctx->pending_redirect->rd_type == REDIRECT_HEREDOC
		 && word->o_quoted
		) {
			ctx->pending_redirect->rd_dup |= HEREDOC_QUOTED;
		}
		word->o_assignment = NOT_ASSIGNMENT;
		debug_printf_parse("word stored in rd_filename: '%s'\n", word->data);
	} else {
		/* "{ echo foo; } echo bar" - bad */
		/* NB: bash allows e.g.:
		 * if true; then { echo foo; } fi
		 * while if false; then false; fi do break; done
		 * and disallows:
		 * while if false; then false; fi; do; break; done
		 * TODO? */
		if (command->group) {
			syntax(word->data);
			debug_printf_parse("done_word return 1: syntax error, "
					"groups and arglists don't mix\n");
			return 1;
		}
#if HAS_KEYWORDS
#if ENABLE_HUSH_CASE
		if (ctx->ctx_dsemicolon
		 && strcmp(word->data, "esac") != 0 /* not "... pattern) cmd;; esac" */
		) {
			/* already done when ctx_dsemicolon was set to 1: */
			/* ctx->ctx_res_w = RES_MATCH; */
			ctx->ctx_dsemicolon = 0;
		} else
#endif
		if (!command->argv /* if it's the first word... */
#if ENABLE_HUSH_LOOPS
		 && ctx->ctx_res_w != RES_FOR /* ...not after FOR or IN */
		 && ctx->ctx_res_w != RES_IN
#endif
		) {
			debug_printf_parse(": checking '%s' for reserved-ness\n", word->data);
			if (reserved_word(word, ctx)) {
				o_reset(word);
				debug_printf_parse("done_word return %d\n",
						(ctx->ctx_res_w == RES_SNTX));
				return (ctx->ctx_res_w == RES_SNTX);
			}
		}
#endif
		if (word->o_quoted /* word had "xx" or 'xx' at least as part of it. */
		 /* optimization: and if it's ("" or '') or ($v... or `cmd`...): */
		 && (word->data[0] == '\0' || word->data[0] == SPECIAL_VAR_SYMBOL)
		 /* (otherwise it's known to be not empty and is already safe) */
		) {
			/* exclude "$@" - it can expand to no word despite "" */
			char *p = word->data;
			while (p[0] == SPECIAL_VAR_SYMBOL
			    && (p[1] & 0x7f) == '@'
			    && p[2] == SPECIAL_VAR_SYMBOL
			) {
				p += 3;
			}
			if (p == word->data || p[0] != '\0') {
				/* saw no "$@", or not only "$@" but some
				 * real text is there too */
				/* insert "empty variable" reference, this makes
				 * e.g. "", $empty"" etc to not disappear */
				o_addchr(word, SPECIAL_VAR_SYMBOL);
				o_addchr(word, SPECIAL_VAR_SYMBOL);
			}
		}
		command->argv = add_string_to_strings(command->argv, xstrdup(word->data));
		debug_print_strings("word appended to argv", command->argv);
	}

	o_reset(word);
	ctx->pending_redirect = NULL;

#if ENABLE_HUSH_LOOPS
	/* Force FOR to have just one word (variable name) */
	/* NB: basically, this makes hush see "for v in ..." syntax as if
	 * as it is "for v; in ...". FOR and IN become two pipe structs
	 * in parse tree. */
	if (ctx->ctx_res_w == RES_FOR) {
//TODO: check that command->argv[0] is a valid variable name!
		done_pipe(ctx, PIPE_SEQ);
	}
#endif
#if ENABLE_HUSH_CASE
	/* Force CASE to have just one word */
	if (ctx->ctx_res_w == RES_CASE) {
		done_pipe(ctx, PIPE_SEQ);
	}
#endif
	debug_printf_parse("done_word return 0\n");
	return 0;
}


/* Peek ahead in the input to find out if we have a "&n" construct,
 * as in "2>&1", that represents duplicating a file descriptor.
 * Return: REDIRFD_CLOSE (-3) if >&- "close fd" construct is seen,
 * -2 (syntax error), -1 if no & was seen, or the number found.
 */
#if BB_MMU
#define redirect_dup_num(as_string, input) \
	redirect_dup_num(input)
#endif
static int redirect_dup_num(o_string *as_string, struct in_str *input)
{
	int ch, d, ok;

	ch = i_peek(input);
	if (ch != '&')
		return -1;

	ch = i_getch(input);  /* get the & */
	nommu_addchr(as_string, ch);
	ch = i_peek(input);
	if (ch == '-') {
		ch = i_getch(input);
		nommu_addchr(as_string, ch);
		return REDIRFD_CLOSE;
	}
	d = 0;
	ok = 0;
	while (ch != EOF && isdigit(ch)) {
		d = d*10 + (ch-'0');
		ok = 1;
		ch = i_getch(input);
		nommu_addchr(as_string, ch);
		ch = i_peek(input);
	}
	if (ok) return d;

//TODO: this is the place to catch ">&file" bashism (redirect both fd 1 and 2)

	bb_error_msg("ambiguous redirect");
	return -2;
}

/* Return code is 0 normally, 1 if a syntax error is detected
 */
static int parse_redirect(struct parse_context *ctx,
		int fd,
		redir_type style,
		struct in_str *input)
{
	struct command *command = ctx->command;
	struct redir_struct *redir;
	struct redir_struct **redirp;
	int dup_num;

	dup_num = -1;
	if (style != REDIRECT_HEREDOC) {
		/* Check for a '2>&1' type redirect */
		dup_num = redirect_dup_num(&ctx->as_string, input);
		if (dup_num == -2)
			return 1;  /* syntax error */
	} else {
		int ch = i_peek(input);
		dup_num = (ch == '-'); /* HEREDOC_SKIPTABS bit is 1 */
		if (dup_num) { /* <<-... */
			ch = i_getch(input);
			nommu_addchr(&ctx->as_string, ch);
			ch = i_peek(input);
		}
		/* <<[-] word is the same as <<[-]word */
		while (ch == ' ' || ch == '\t') {
			ch = i_getch(input);
			nommu_addchr(&ctx->as_string, ch);
			ch = i_peek(input);
		}
	}

	if (style == REDIRECT_OVERWRITE && dup_num == -1) {
		int ch = i_peek(input);
		if (ch == '|') {
			/* >|FILE redirect ("clobbering" >).
			 * Since we do not support "set -o noclobber" yet,
			 * >| and > are the same for now. Just eat |.
			 */
			ch = i_getch(input);
			nommu_addchr(&ctx->as_string, ch);
		}
	}

	/* Create a new redir_struct and append it to the linked list */
	redirp = &command->redirects;
	while ((redir = *redirp) != NULL) {
		redirp = &(redir->next);
	}
	*redirp = redir = xzalloc(sizeof(*redir));
	/* redir->next = NULL; */
	/* redir->rd_filename = NULL; */
	redir->rd_type = style;
	redir->rd_fd = (fd == -1) ? redir_table[style].default_fd : fd;

	debug_printf_parse("redirect type %d %s\n", redir->rd_fd,
				redir_table[style].descrip);

	redir->rd_dup = dup_num;
	if (style != REDIRECT_HEREDOC && dup_num != -1) {
		/* Erik had a check here that the file descriptor in question
		 * is legit; I postpone that to "run time"
		 * A "-" representation of "close me" shows up as a -3 here */
		debug_printf_parse("duplicating redirect '%d>&%d'\n",
				redir->rd_fd, redir->rd_dup);
	} else {
		/* Set ctx->pending_redirect, so we know what to do at the
		 * end of the next parsed word. */
		ctx->pending_redirect = redir;
	}
	return 0;
}

/* If a redirect is immediately preceded by a number, that number is
 * supposed to tell which file descriptor to redirect.  This routine
 * looks for such preceding numbers.  In an ideal world this routine
 * needs to handle all the following classes of redirects...
 *     echo 2>foo     # redirects fd  2 to file "foo", nothing passed to echo
 *     echo 49>foo    # redirects fd 49 to file "foo", nothing passed to echo
 *     echo -2>foo    # redirects fd  1 to file "foo",    "-2" passed to echo
 *     echo 49x>foo   # redirects fd  1 to file "foo",   "49x" passed to echo
 *
 * http://www.opengroup.org/onlinepubs/009695399/utilities/xcu_chap02.html
 * "2.7 Redirection
 * ... If n is quoted, the number shall not be recognized as part of
 * the redirection expression. For example:
 * echo \2>a
 * writes the character 2 into file a"
 * We are getting it right by setting ->o_quoted on any \<char>
 *
 * A -1 return means no valid number was found,
 * the caller should use the appropriate default for this redirection.
 */
static int redirect_opt_num(o_string *o)
{
	int num;

	if (o->data == NULL)
		return -1;
	num = bb_strtou(o->data, NULL, 10);
	if (errno || num < 0)
		return -1;
	o_reset(o);
	return num;
}

#if BB_MMU
#define fetch_till_str(as_string, input, word, skip_tabs) \
	fetch_till_str(input, word, skip_tabs)
#endif
static char *fetch_till_str(o_string *as_string,
		struct in_str *input,
		const char *word,
		int skip_tabs)
{
	o_string heredoc = NULL_O_STRING;
	int past_EOL = 0;
	int ch;

	goto jump_in;
	while (1) {
		ch = i_getch(input);
		nommu_addchr(as_string, ch);
		if (ch == '\n') {
			if (strcmp(heredoc.data + past_EOL, word) == 0) {
				heredoc.data[past_EOL] = '\0';
				debug_printf_parse("parsed heredoc '%s'\n", heredoc.data);
				return heredoc.data;
			}
			do {
				o_addchr(&heredoc, ch);
				past_EOL = heredoc.length;
 jump_in:
				do {
					ch = i_getch(input);
					nommu_addchr(as_string, ch);
				} while (skip_tabs && ch == '\t');
			} while (ch == '\n');
		}
		if (ch == EOF) {
			o_free_unsafe(&heredoc);
			return NULL;
		}
		o_addchr(&heredoc, ch);
		nommu_addchr(as_string, ch);
	}
}

/* Look at entire parse tree for not-yet-loaded REDIRECT_HEREDOCs
 * and load them all. There should be exactly heredoc_cnt of them.
 */
static int fetch_heredocs(int heredoc_cnt, struct parse_context *ctx, struct in_str *input)
{
	struct pipe *pi = ctx->list_head;

	while (pi) {
		int i;
		struct command *cmd = pi->cmds;

		debug_printf_parse("fetch_heredocs: num_cmds:%d cmd argv0:'%s'\n",
				pi->num_cmds,
				cmd->argv ? cmd->argv[0] : "NONE");
		for (i = 0; i < pi->num_cmds; i++) {
			struct redir_struct *redir = cmd->redirects;

			debug_printf_parse("fetch_heredocs: %d cmd argv0:'%s'\n",
					i, cmd->argv ? cmd->argv[0] : "NONE");
			while (redir) {
				if (redir->rd_type == REDIRECT_HEREDOC) {
					char *p;

					if (heredoc_cnt <= 0) {
						syntax("heredoc BUG 1");
						return 1; /* error */
					}
					redir->rd_type = REDIRECT_HEREDOC2;
					/* redir->dup is (ab)used to indicate <<- */
					p = fetch_till_str(&ctx->as_string, input,
						redir->rd_filename, redir->rd_dup & HEREDOC_SKIPTABS);
					if (!p) {
						syntax("unexpected EOF in here document");
						return 1;
					}
					free(redir->rd_filename);
					redir->rd_filename = p;
					heredoc_cnt--;
				}
				redir = redir->next;
			}
			cmd++;
		}
		pi = pi->next;
	}
	/* Should be 0. If it isn't, it's a parse error */
	if (heredoc_cnt)
		syntax("heredoc BUG 2");
	return heredoc_cnt;
}


#if BB_MMU
#define parse_stream(pstring, input, end_trigger) \
	parse_stream(input, end_trigger)
#endif
static struct pipe *parse_stream(char **pstring,
		struct in_str *input,
		int end_trigger);
static void parse_and_run_string(const char *s);

#if ENABLE_HUSH_TICK
static FILE *generate_stream_from_string(const char *s)
{
	FILE *pf;
	int pid, channel[2];

	xpipe(channel);
	pid = BB_MMU ? fork() : vfork();
	if (pid < 0)
		bb_perror_msg_and_die(BB_MMU ? "fork" : "vfork");

	if (pid == 0) { /* child */
		disable_restore_tty_pgrp_on_exit();
		/* Process substitution is not considered to be usual
		 * 'command execution'.
		 * SUSv3 says ctrl-Z should be ignored, ctrl-C should not.
		 */
		bb_signals(0
			+ (1 << SIGTSTP)
			+ (1 << SIGTTIN)
			+ (1 << SIGTTOU)
			, SIG_IGN);
		close(channel[0]); /* NB: close _first_, then move fd! */
		xmove_fd(channel[1], 1);
		/* Prevent it from trying to handle ctrl-z etc */
		USE_HUSH_JOB(G.run_list_level = 1;)
#if BB_MMU
		reset_traps_to_defaults();
		parse_and_run_string(s);
		_exit(G.last_exitcode);
#else
	/* We re-execute after vfork on NOMMU. This makes this script safe:
	 * yes "0123456789012345678901234567890" | dd bs=32 count=64k >BIG
	 * huge=`cat BIG` # was blocking here forever
	 * echo OK
	 */
		re_execute_shell(s, 0);
#endif
	}

	/* parent */
	enable_restore_tty_pgrp_on_exit();
	clean_up_after_re_execute();
	close(channel[1]);
	pf = fdopen(channel[0], "r");
	return pf;
}

/* Return code is exit status of the process that is run. */
static int process_command_subs(o_string *dest, const char *s)
{
	FILE *pf;
	struct in_str pipe_str;
	int ch, eol_cnt;

	pf = generate_stream_from_string(s);
	if (pf == NULL)
		return 1;
	close_on_exec_on(fileno(pf));

	/* Now send results of command back into original context */
	setup_file_in_str(&pipe_str, pf);
	eol_cnt = 0;
	while ((ch = i_getch(&pipe_str)) != EOF) {
		if (ch == '\n') {
			eol_cnt++;
			continue;
		}
		while (eol_cnt) {
			o_addchr(dest, '\n');
			eol_cnt--;
		}
		o_addQchr(dest, ch);
	}

	debug_printf("done reading from pipe, pclose()ing\n");
	/* Note: we got EOF, and we just close the read end of the pipe.
	 * We do not wait for the `cmd` child to terminate. bash and ash do.
	 * Try these:
	 * echo `echo Hi; exec 1>&-; sleep 2` - bash waits 2 sec
	 * `false`; echo $? - bash outputs "1"
	 */
	fclose(pf);
	debug_printf("closed FILE from child. return 0\n");
	return 0;
}
#endif

static int parse_group(o_string *dest, struct parse_context *ctx,
	struct in_str *input, int ch)
{
	/* dest contains characters seen prior to ( or {.
	 * Typically it's empty, but for function defs,
	 * it contains function name (without '()'). */
	struct pipe *pipe_list;
	int endch;
	struct command *command = ctx->command;

	debug_printf_parse("parse_group entered\n");
#if ENABLE_HUSH_FUNCTIONS
	if (ch == 'F') { /* function definition? */
		bb_error_msg("aha '%s' is a function, parsing it...", dest->data);
		//command->fname = dest->data;
		command->grp_type = GRP_FUNCTION;
//TODO: review every o_reset() location... do they handle all o_string fields correctly?
		memset(dest, 0, sizeof(*dest));
	}
#endif
	if (command->argv /* word [word](... */
	 || dest->length /* word(... */
	 || dest->o_quoted /* ""(... */
	) {
		syntax(NULL);
		debug_printf_parse("parse_group return 1: "
			"syntax error, groups and arglists don't mix\n");
		return 1;
	}
	endch = '}';
	if (ch == '(') {
		endch = ')';
		command->grp_type = GRP_SUBSHELL;
	}
	{
#if !BB_MMU
		char *as_string = NULL;
#endif
		pipe_list = parse_stream(&as_string, input, endch);
#if !BB_MMU
		if (as_string)
			o_addstr(&ctx->as_string, as_string);
#endif
		/* empty ()/{} or parse error? */
		if (!pipe_list || pipe_list == ERR_PTR) {
#if !BB_MMU
			free(as_string);
#endif
			syntax(NULL);
			debug_printf_parse("parse_group return 1: "
				"parse_stream returned %p\n", pipe_list);
			return 1;
		}
		command->group = pipe_list;
#if !BB_MMU
		as_string[strlen(as_string) - 1] = '\0'; /* plink ')' or '}' */
		command->group_as_string = as_string;
		debug_printf_parse("end of group, remembering as:'%s'\n",
				command->group_as_string);
#endif
	}
	debug_printf_parse("parse_group return 0\n");
	return 0;
	/* command remains "open", available for possible redirects */
}

#if ENABLE_HUSH_TICK || ENABLE_SH_MATH_SUPPORT
/* Subroutines for copying $(...) and `...` things */
static int add_till_backquote(o_string *dest, struct in_str *input);
/* '...' */
static int add_till_single_quote(o_string *dest, struct in_str *input)
{
	while (1) {
		int ch = i_getch(input);
		if (ch == EOF) {
			syntax("unterminated '");
			return 1;
		}
		if (ch == '\'')
			return 0;
		o_addchr(dest, ch);
	}
}
/* "...\"...`..`...." - do we need to handle "...$(..)..." too? */
static int add_till_double_quote(o_string *dest, struct in_str *input)
{
	while (1) {
		int ch = i_getch(input);
		if (ch == EOF) {
			syntax("unterminated \"");
			return 1;
		}
		if (ch == '"')
			return 0;
		if (ch == '\\') {  /* \x. Copy both chars. */
			o_addchr(dest, ch);
			ch = i_getch(input);
		}
		o_addchr(dest, ch);
		if (ch == '`') {
			if (add_till_backquote(dest, input))
				return 1;
			o_addchr(dest, ch);
			continue;
		}
		//if (ch == '$') ...
	}
}
/* Process `cmd` - copy contents until "`" is seen. Complicated by
 * \` quoting.
 * "Within the backquoted style of command substitution, backslash
 * shall retain its literal meaning, except when followed by: '$', '`', or '\'.
 * The search for the matching backquote shall be satisfied by the first
 * backquote found without a preceding backslash; during this search,
 * if a non-escaped backquote is encountered within a shell comment,
 * a here-document, an embedded command substitution of the $(command)
 * form, or a quoted string, undefined results occur. A single-quoted
 * or double-quoted string that begins, but does not end, within the
 * "`...`" sequence produces undefined results."
 * Example                               Output
 * echo `echo '\'TEST\`echo ZZ\`BEST`    \TESTZZBEST
 */
static int add_till_backquote(o_string *dest, struct in_str *input)
{
	while (1) {
		int ch = i_getch(input);
		if (ch == EOF) {
			syntax("unterminated `");
			return 1;
		}
		if (ch == '`')
			return 0;
		if (ch == '\\') {
			/* \x. Copy both chars unless it is \` */
			int ch2 = i_getch(input);
			if (ch2 == EOF) {
				syntax("unterminated `");
				return 1;
			}
			if (ch2 != '`' && ch2 != '$' && ch2 != '\\')
				o_addchr(dest, ch);
			ch = ch2;
		}
		o_addchr(dest, ch);
	}
}
/* Process $(cmd) - copy contents until ")" is seen. Complicated by
 * quoting and nested ()s.
 * "With the $(command) style of command substitution, all characters
 * following the open parenthesis to the matching closing parenthesis
 * constitute the command. Any valid shell script can be used for command,
 * except a script consisting solely of redirections which produces
 * unspecified results."
 * Example                              Output
 * echo $(echo '(TEST)' BEST)           (TEST) BEST
 * echo $(echo 'TEST)' BEST)            TEST) BEST
 * echo $(echo \(\(TEST\) BEST)         ((TEST) BEST
 */
static int add_till_closing_paren(o_string *dest, struct in_str *input, bool dbl)
{
	int count = 0;
	while (1) {
		int ch = i_getch(input);
		if (ch == EOF) {
			syntax("unterminated )");
			return 1;
		}
		if (ch == '(')
			count++;
		if (ch == ')') {
			if (--count < 0) {
				if (!dbl)
					break;
				if (i_peek(input) == ')') {
					i_getch(input);
					break;
				}
			}
		}
		o_addchr(dest, ch);
		if (ch == '\'') {
			if (add_till_single_quote(dest, input))
				return 1;
			o_addchr(dest, ch);
			continue;
		}
		if (ch == '"') {
			if (add_till_double_quote(dest, input))
				return 1;
			o_addchr(dest, ch);
			continue;
		}
		if (ch == '\\') {
			/* \x. Copy verbatim. Important for  \(, \) */
			ch = i_getch(input);
			if (ch == EOF) {
				syntax("unterminated )");
				return 1;
			}
			o_addchr(dest, ch);
			continue;
		}
	}
	return 0;
}
#endif /* ENABLE_HUSH_TICK || ENABLE_SH_MATH_SUPPORT */

/* Return code: 0 for OK, 1 for syntax error */
#if BB_MMU
#define handle_dollar(as_string, dest, input) \
	handle_dollar(dest, input)
#endif
static int handle_dollar(o_string *as_string,
		o_string *dest,
		struct in_str *input)
{
	int expansion;
	int ch = i_peek(input);  /* first character after the $ */
	unsigned char quote_mask = dest->o_escape ? 0x80 : 0;

	debug_printf_parse("handle_dollar entered: ch='%c'\n", ch);
	if (isalpha(ch)) {
		ch = i_getch(input);
		nommu_addchr(as_string, ch);
 make_var:
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
		while (1) {
			debug_printf_parse(": '%c'\n", ch);
			o_addchr(dest, ch | quote_mask);
			quote_mask = 0;
			ch = i_peek(input);
			if (!isalnum(ch) && ch != '_')
				break;
			ch = i_getch(input);
			nommu_addchr(as_string, ch);
		}
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
	} else if (isdigit(ch)) {
 make_one_char_var:
		ch = i_getch(input);
		nommu_addchr(as_string, ch);
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
		debug_printf_parse(": '%c'\n", ch);
		o_addchr(dest, ch | quote_mask);
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
	} else switch (ch) {
	case '$': /* pid */
	case '!': /* last bg pid */
	case '?': /* last exit code */
	case '#': /* number of args */
	case '*': /* args */
	case '@': /* args */
		goto make_one_char_var;
	case '{': {
		bool first_char, all_digits;

		o_addchr(dest, SPECIAL_VAR_SYMBOL);
		ch = i_getch(input);
		nommu_addchr(as_string, ch);
		/* XXX maybe someone will try to escape the '}' */
		expansion = 0;
		first_char = true;
		all_digits = false;
		while (1) {
			ch = i_getch(input);
			nommu_addchr(as_string, ch);
			if (ch == '}')
				break;

			if (first_char) {
				if (ch == '#')
					/* ${#var}: length of var contents */
					goto char_ok;
				else if (isdigit(ch)) {
					all_digits = true;
					goto char_ok;
				}
			}

			if (expansion < 2
			 && (  (all_digits && !isdigit(ch))
			    || (!all_digits && !isalnum(ch) && ch != '_')
			    )
			) {
				/* handle parameter expansions
				 * http://www.opengroup.org/onlinepubs/009695399/utilities/xcu_chap02.html#tag_02_06_02
				 */
				if (first_char)
					goto case_default;
				switch (ch) {
				case ':': /* null modifier */
					if (expansion == 0) {
						debug_printf_parse(": null modifier\n");
						++expansion;
						break;
					}
					goto case_default;
				case '#': /* remove prefix */
				case '%': /* remove suffix */
					if (expansion == 0) {
						debug_printf_parse(": remove suffix/prefix\n");
						expansion = 2;
						break;
					}
					goto case_default;
				case '-': /* default value */
				case '=': /* assign default */
				case '+': /* alternative */
				case '?': /* error indicate */
					debug_printf_parse(": parameter expansion\n");
					expansion = 2;
					break;
				default:
				case_default:
					syntax("unterminated ${name}");
					debug_printf_parse("handle_dollar return 1: unterminated ${name}\n");
					return 1;
				}
			}
 char_ok:
			debug_printf_parse(": '%c'\n", ch);
			o_addchr(dest, ch | quote_mask);
			quote_mask = 0;
			first_char = false;
		}
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
		break;
	}
#if (ENABLE_SH_MATH_SUPPORT || ENABLE_HUSH_TICK)
	case '(': {
# if !BB_MMU
		int pos;
# endif
		ch = i_getch(input);
		nommu_addchr(as_string, ch);
# if ENABLE_SH_MATH_SUPPORT
		if (i_peek(input) == '(') {
			ch = i_getch(input);
			nommu_addchr(as_string, ch);
			o_addchr(dest, SPECIAL_VAR_SYMBOL);
			o_addchr(dest, /*quote_mask |*/ '+');
#  if !BB_MMU
			pos = dest->length;
#  endif
			if (add_till_closing_paren(dest, input, true))
				return 1;
#  if !BB_MMU
			if (as_string) {
				o_addstr(as_string, dest->data + pos);
				o_addchr(as_string, ')');
				o_addchr(as_string, ')');
			}
#  endif
			o_addchr(dest, SPECIAL_VAR_SYMBOL);
			break;
		}
# endif
# if ENABLE_HUSH_TICK
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
		o_addchr(dest, quote_mask | '`');
#  if !BB_MMU
		pos = dest->length;
#  endif
		if (add_till_closing_paren(dest, input, false))
			return 1;
#  if !BB_MMU
		if (as_string) {
			o_addstr(as_string, dest->data + pos);
			o_addchr(as_string, '`');
		}
#  endif
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
# endif
		break;
	}
#endif
	case '_':
		ch = i_getch(input);
		nommu_addchr(as_string, ch);
		ch = i_peek(input);
		if (isalnum(ch)) { /* it's $_name or $_123 */
			ch = '_';
			goto make_var;
		}
		/* else: it's $_ */
	/* TODO: */
	/* $_ Shell or shell script name; or last cmd name */
	/* $- Option flags set by set builtin or shell options (-i etc) */
	default:
		o_addQchr(dest, '$');
	}
	debug_printf_parse("handle_dollar return 0\n");
	return 0;
}

#if BB_MMU
#define parse_stream_dquoted(as_string, dest, input, dquote_end) \
	parse_stream_dquoted(dest, input, dquote_end)
#endif
static int parse_stream_dquoted(o_string *as_string,
		o_string *dest,
		struct in_str *input,
		int dquote_end)
{
	int ch;
	int next;

 again:
	ch = i_getch(input);
	if (ch != EOF)
		nommu_addchr(as_string, ch);
	if (ch == dquote_end) { /* may be only '"' or EOF */
		if (dest->o_assignment == NOT_ASSIGNMENT)
			dest->o_escape ^= 1;
		debug_printf_parse("parse_stream_dquoted return 0\n");
		return 0;
	}
	/* note: can't move it above ch == dquote_end check! */
	if (ch == EOF) {
		syntax("unterminated \"");
		debug_printf_parse("parse_stream_dquoted return 1: unterminated \"\n");
		return 1;
	}
	next = '\0';
	if (ch != '\n') {
		next = i_peek(input);
	}
	debug_printf_parse(": ch=%c (%d) escape=%d\n",
					ch, ch, dest->o_escape);
	if (ch == '\\') {
		if (next == EOF) {
			syntax("\\<eof>");
			debug_printf_parse("parse_stream_dquoted return 1: \\<eof>\n");
			return 1;
		}
		/* bash:
		 * "The backslash retains its special meaning [in "..."]
		 * only when followed by one of the following characters:
		 * $, `, ", \, or <newline>.  A double quote may be quoted
		 * within double quotes by preceding it with a backslash.
		 * If enabled, history expansion will be performed unless
		 * an ! appearing in double quotes is escaped using
		 * a backslash. The backslash preceding the ! is not removed."
		 */
		if (strchr("$`\"\\", next) != NULL) {
			o_addqchr(dest, i_getch(input));
		} else {
			o_addqchr(dest, '\\');
		}
		goto again;
	}
	if (ch == '$') {
		if (handle_dollar(as_string, dest, input) != 0) {
			debug_printf_parse("parse_stream_dquoted return 1: "
					"handle_dollar returned non-0\n");
			return 1;
		}
		goto again;
	}
#if ENABLE_HUSH_TICK
	if (ch == '`') {
		//int pos = dest->length;
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
		o_addchr(dest, 0x80 | '`');
		if (add_till_backquote(dest, input))
			return 1;
		o_addchr(dest, SPECIAL_VAR_SYMBOL);
		//debug_printf_subst("SUBST RES3 '%s'\n", dest->data + pos);
		goto again;
	}
#endif
	o_addQchr(dest, ch);
	if (ch == '='
	 && (dest->o_assignment == MAYBE_ASSIGNMENT
	    || dest->o_assignment == WORD_IS_KEYWORD)
	 && is_assignment(dest->data)
	) {
		dest->o_assignment = DEFINITELY_ASSIGNMENT;
	}
	goto again;
}

/*
 * Scan input until EOF or end_trigger char.
 * Return a list of pipes to execute, or NULL on EOF
 * or if end_trigger character is met.
 * On syntax error, exit is shell is not interactive,
 * reset parsing machinery and start parsing anew,
 * or return ERR_PTR.
 */
static struct pipe *parse_stream(char **pstring,
		struct in_str *input,
		int end_trigger)
{
	struct parse_context ctx;
	o_string dest = NULL_O_STRING;
	int is_in_dquote;
	int heredoc_cnt;

	/* Double-quote state is handled in the state variable is_in_dquote.
	 * A single-quote triggers a bypass of the main loop until its mate is
	 * found.  When recursing, quote state is passed in via dest->o_escape.
	 */
	debug_printf_parse("parse_stream entered, end_trigger='%c'\n",
			end_trigger ? : 'X');

	G.ifs = get_local_var_value("IFS");
	if (G.ifs == NULL)
		G.ifs = " \t\n";

 reset:
#if ENABLE_HUSH_INTERACTIVE
	input->promptmode = 0; /* PS1 */
#endif
	/* dest.o_assignment = MAYBE_ASSIGNMENT; - already is */
	initialize_context(&ctx);
	is_in_dquote = 0;
	heredoc_cnt = 0;
	while (1) {
		const char *is_ifs;
		const char *is_special;
		int ch;
		int next;
		int redir_fd;
		redir_type redir_style;

		if (is_in_dquote) {
			/* dest.o_quoted = 1; - already is (see below) */
			if (parse_stream_dquoted(&ctx.as_string, &dest, input, '"')) {
				goto parse_error;
			}
			/* We reached closing '"' */
			is_in_dquote = 0;
		}
		ch = i_getch(input);
		debug_printf_parse(": ch=%c (%d) escape=%d\n",
						ch, ch, dest.o_escape);
		if (ch == EOF) {
			struct pipe *pi;

			if (heredoc_cnt) {
				syntax("unterminated here document");
				goto parse_error;
			}
			if (done_word(&dest, &ctx)) {
				goto parse_error;
			}
			o_free(&dest);
			done_pipe(&ctx, PIPE_SEQ);
			pi = ctx.list_head;
			/* If we got nothing... */
// TODO: test script consisting of just "&"
			if (pi->num_cmds == 0
			    IF_HAS_KEYWORDS( && pi->res_word == RES_NONE)
			) {
				free_pipe_list(pi, 0);
				pi = NULL;
			}
			debug_printf_parse("parse_stream return %p\n", pi);
#if !BB_MMU
			debug_printf_parse("as_string '%s'\n", ctx.as_string.data);
			if (pstring)
				*pstring = ctx.as_string.data;
			else
				o_free_unsafe(&ctx.as_string);
#endif
			return pi;
		}
		nommu_addchr(&ctx.as_string, ch);
		is_ifs = strchr(G.ifs, ch);
		is_special = strchr("<>;&|(){}#'" /* special outside of "str" */
				"\\$\"" USE_HUSH_TICK("`") /* always special */
				, ch);

		if (!is_special && !is_ifs) { /* ordinary char */
			o_addQchr(&dest, ch);
			if ((dest.o_assignment == MAYBE_ASSIGNMENT
			    || dest.o_assignment == WORD_IS_KEYWORD)
			 && ch == '='
			 && is_assignment(dest.data)
			) {
				dest.o_assignment = DEFINITELY_ASSIGNMENT;
			}
			continue;
		}

		if (is_ifs) {
			if (done_word(&dest, &ctx)) {
				goto parse_error;
			}
			if (ch == '\n') {
#if ENABLE_HUSH_CASE
				/* "case ... in <newline> word) ..." -
				 * newlines are ignored (but ';' wouldn't be) */
				if (ctx.command->argv == NULL
				 && ctx.ctx_res_w == RES_MATCH
				) {
					continue;
				}
#endif
				/* Treat newline as a command separator. */
				done_pipe(&ctx, PIPE_SEQ);
				debug_printf_parse("heredoc_cnt:%d\n", heredoc_cnt);
				if (heredoc_cnt) {
					if (fetch_heredocs(heredoc_cnt, &ctx, input)) {
						goto parse_error;
					}
					heredoc_cnt = 0;
				}
				dest.o_assignment = MAYBE_ASSIGNMENT;
				ch = ';';
				/* note: if (is_ifs) continue;
				 * will still trigger for us */
			}
		}
		if (end_trigger && end_trigger == ch
		 && (heredoc_cnt == 0 || end_trigger != ';')
		) {
//TODO: disallow "{ cmd }" without semicolon
			if (heredoc_cnt) {
				/* This is technically valid:
				 * { cat <<HERE; }; echo Ok
				 * heredoc
				 * heredoc
				 * heredoc
				 * HERE
				 * but we don't support this.
				 * We require heredoc to be in enclosing {}/(),
				 * if any.
				 */
				syntax("unterminated here document");
				goto parse_error;
			}
			if (done_word(&dest, &ctx)) {
				goto parse_error;
			}
			done_pipe(&ctx, PIPE_SEQ);
			dest.o_assignment = MAYBE_ASSIGNMENT;
			/* Do we sit outside of any if's, loops or case's? */
			if (!HAS_KEYWORDS
			 IF_HAS_KEYWORDS(|| (ctx.ctx_res_w == RES_NONE && ctx.old_flag == 0))
			) {
				debug_printf_parse("parse_stream return %p: "
						"end_trigger char found\n",
						ctx.list_head);
				o_free(&dest);
#if !BB_MMU
				debug_printf_parse("as_string '%s'\n", ctx.as_string.data);
				if (pstring)
					*pstring = ctx.as_string.data;
				else
					o_free_unsafe(&ctx.as_string);
#endif
				return ctx.list_head;
			}
		}
		if (is_ifs)
			continue;

		if (dest.o_assignment == MAYBE_ASSIGNMENT) {
			/* ch is a special char and thus this word
			 * cannot be an assignment */
			dest.o_assignment = NOT_ASSIGNMENT;
		}

		next = '\0';
		if (ch != '\n') {
			next = i_peek(input);
		}

		switch (ch) {
		case '#':
			if (dest.length == 0) {
				while (1) {
					ch = i_peek(input);
					if (ch == EOF || ch == '\n')
						break;
					i_getch(input);
					/* note: we do not add it to &ctx.as_string */
				}
				nommu_addchr(&ctx.as_string, '\n');
			} else {
				o_addQchr(&dest, ch);
			}
			break;
		case '\\':
			if (next == EOF) {
				syntax("\\<eof>");
				goto parse_error;
			}
			o_addchr(&dest, '\\');
			ch = i_getch(input);
			nommu_addchr(&ctx.as_string, ch);
			o_addchr(&dest, ch);
			/* Example: echo Hello \2>file
			 * we need to know that word 2 is quoted */
			dest.o_quoted = 1;
			break;
		case '$':
			if (handle_dollar(&ctx.as_string, &dest, input) != 0) {
				debug_printf_parse("parse_stream parse error: "
					"handle_dollar returned non-0\n");
				goto parse_error;
			}
			break;
		case '\'':
			dest.o_quoted = 1;
			while (1) {
				ch = i_getch(input);
				if (ch == EOF) {
					syntax("unterminated '");
					goto parse_error;
				}
				nommu_addchr(&ctx.as_string, ch);
				if (ch == '\'')
					break;
				if (dest.o_assignment == NOT_ASSIGNMENT)
					o_addqchr(&dest, ch);
				else
					o_addchr(&dest, ch);
			}
			break;
		case '"':
			dest.o_quoted = 1;
			is_in_dquote ^= 1; /* invert */
			if (dest.o_assignment == NOT_ASSIGNMENT)
				dest.o_escape ^= 1;
			break;
#if ENABLE_HUSH_TICK
		case '`': {
#if !BB_MMU
			int pos;
#endif
			o_addchr(&dest, SPECIAL_VAR_SYMBOL);
			o_addchr(&dest, '`');
#if !BB_MMU
			pos = dest.length;
#endif
			if (add_till_backquote(&dest, input))
				goto parse_error;
#if !BB_MMU
			o_addstr(&ctx.as_string, dest.data + pos);
			o_addchr(&ctx.as_string, '`');
#endif
			o_addchr(&dest, SPECIAL_VAR_SYMBOL);
			//debug_printf_subst("SUBST RES3 '%s'\n", dest.data + pos);
			break;
		}
#endif
		case '>':
			redir_fd = redirect_opt_num(&dest);
			if (done_word(&dest, &ctx)) {
				goto parse_error;
			}
			redir_style = REDIRECT_OVERWRITE;
			if (next == '>') {
				redir_style = REDIRECT_APPEND;
				ch = i_getch(input);
				nommu_addchr(&ctx.as_string, ch);
			}
#if 0
			else if (next == '(') {
				syntax(">(process) not supported");
				goto parse_error;
			}
#endif
			if (parse_redirect(&ctx, redir_fd, redir_style, input))
				goto parse_error;
			break;
		case '<':
			redir_fd = redirect_opt_num(&dest);
			if (done_word(&dest, &ctx)) {
				goto parse_error;
			}
			redir_style = REDIRECT_INPUT;
			if (next == '<') {
				redir_style = REDIRECT_HEREDOC;
				heredoc_cnt++;
				debug_printf_parse("++heredoc_cnt=%d\n", heredoc_cnt);
				ch = i_getch(input);
				nommu_addchr(&ctx.as_string, ch);
			} else if (next == '>') {
				redir_style = REDIRECT_IO;
				ch = i_getch(input);
				nommu_addchr(&ctx.as_string, ch);
			}
#if 0
			else if (next == '(') {
				syntax("<(process) not supported");
				goto parse_error;
			}
#endif
			if (parse_redirect(&ctx, redir_fd, redir_style, input))
				goto parse_error;
			break;
		case ';':
#if ENABLE_HUSH_CASE
 case_semi:
#endif
			if (done_word(&dest, &ctx)) {
				goto parse_error;
			}
			done_pipe(&ctx, PIPE_SEQ);
#if ENABLE_HUSH_CASE
			/* Eat multiple semicolons, detect
			 * whether it means something special */
			while (1) {
				ch = i_peek(input);
				if (ch != ';')
					break;
				ch = i_getch(input);
				nommu_addchr(&ctx.as_string, ch);
				if (ctx.ctx_res_w == RES_CASEI) {
					ctx.ctx_dsemicolon = 1;
					ctx.ctx_res_w = RES_MATCH;
					break;
				}
			}
#endif
 new_cmd:
			/* We just finished a cmd. New one may start
			 * with an assignment */
			dest.o_assignment = MAYBE_ASSIGNMENT;
			break;
		case '&':
			if (done_word(&dest, &ctx)) {
				goto parse_error;
			}
			if (next == '&') {
				ch = i_getch(input);
				nommu_addchr(&ctx.as_string, ch);
				done_pipe(&ctx, PIPE_AND);
			} else {
				done_pipe(&ctx, PIPE_BG);
			}
			goto new_cmd;
		case '|':
			if (done_word(&dest, &ctx)) {
				goto parse_error;
			}
#if ENABLE_HUSH_CASE
			if (ctx.ctx_res_w == RES_MATCH)
				break; /* we are in case's "word | word)" */
#endif
			if (next == '|') { /* || */
				ch = i_getch(input);
				nommu_addchr(&ctx.as_string, ch);
				done_pipe(&ctx, PIPE_OR);
			} else {
				/* we could pick up a file descriptor choice here
				 * with redirect_opt_num(), but bash doesn't do it.
				 * "echo foo 2| cat" yields "foo 2". */
				done_command(&ctx);
			}
			goto new_cmd;
		case '(':
#if ENABLE_HUSH_CASE
			/* "case... in [(]word)..." - skip '(' */
			if (ctx.ctx_res_w == RES_MATCH
			 && ctx.command->argv == NULL /* not (word|(... */
			 && dest.length == 0 /* not word(... */
			 && dest.o_quoted == 0 /* not ""(... */
			) {
				continue;
			}
#endif
#if ENABLE_HUSH_FUNCTIONS
			if (dest.length != 0 /* not just () but word() */
			 && dest.o_quoted == 0 /* not a"b"c() */
			 && ctx.command->argv == NULL /* it's the first word */
//TODO: "func ( ) {...}" - note spaces - is valid format too in bash
			 && i_peek(input) == ')'
			 && !match_reserved_word(&dest)
			) {
				bb_error_msg("seems like a function definition");
				i_getch(input);
//if !BB_MMU o_addchr(&ctx.as_string...
				do {
//TODO: do it properly.
					ch = i_getch(input);
				} while (ch == ' ' || ch == '\n');
				if (ch != '{') {
					syntax("was expecting {");
					goto parse_error;
				}
				ch = 'F'; /* magic value */
			}
#endif
		case '{':
			if (parse_group(&dest, &ctx, input, ch) != 0) {
				goto parse_error;
			}
			goto new_cmd;
		case ')':
#if ENABLE_HUSH_CASE
			if (ctx.ctx_res_w == RES_MATCH)
				goto case_semi;
#endif
		case '}':
			/* proper use of this character is caught by end_trigger:
			 * if we see {, we call parse_group(..., end_trigger='}')
			 * and it will match } earlier (not here). */
			syntax("unexpected } or )");
			goto parse_error;
		default:
			if (HUSH_DEBUG)
				bb_error_msg_and_die("BUG: unexpected %c\n", ch);
		}
	} /* while (1) */

 parse_error:
	{
		struct parse_context *pctx;
		IF_HAS_KEYWORDS(struct parse_context *p2;)

		/* Clean up allocated tree.
		 * Samples for finding leaks on syntax error recovery path.
		 * Run them from interactive shell, watch pmap `pidof hush`.
		 * while if false; then false; fi do break; done
		 * (bash accepts it)
		 * while if false; then false; fi; do break; fi
		 * Samples to catch leaks at execution:
		 * while if (true | {true;}); then echo ok; fi; do break; done
		 * while if (true | {true;}); then echo ok; fi; do (if echo ok; break; then :; fi) | cat; break; done
		 */
		pctx = &ctx;
		do {
			/* Update pipe/command counts,
			 * otherwise freeing may miss some */
			done_pipe(pctx, PIPE_SEQ);
			debug_printf_clean("freeing list %p from ctx %p\n",
					pctx->list_head, pctx);
			debug_print_tree(pctx->list_head, 0);
			free_pipe_list(pctx->list_head, 0);
			debug_printf_clean("freed list %p\n", pctx->list_head);
#if !BB_MMU
			o_free_unsafe(&pctx->as_string);
#endif
			IF_HAS_KEYWORDS(p2 = pctx->stack;)
			if (pctx != &ctx) {
				free(pctx);
			}
			IF_HAS_KEYWORDS(pctx = p2;)
		} while (HAS_KEYWORDS && pctx);
		/* Free text, clear all dest fields */
		o_free(&dest);
		/* If we are not in top-level parse, we return,
		 * our caller will propagate error.
		 */
		if (end_trigger != ';') {
#if !BB_MMU
			if (pstring)
				*pstring = NULL;
#endif
			return ERR_PTR;
		}
		/* Discard cached input, force prompt */
		input->p = NULL;
		USE_HUSH_INTERACTIVE(input->promptme = 1;)
		goto reset;
	}
}

/* Executing from string: eval, sh -c '...'
 *          or from file: /etc/profile, . file, sh <script>, sh (intereactive)
 * end_trigger controls how often we stop parsing
 * NUL: parse all, execute, return
 * ';': parse till ';' or newline, execute, repeat till EOF
 */
static void parse_and_run_stream(struct in_str *inp, int end_trigger)
{
	while (1) {
		struct pipe *pipe_list;

		pipe_list = parse_stream(NULL, inp, end_trigger);
		if (!pipe_list) /* EOF */
			break;
		debug_print_tree(pipe_list, 0);
		debug_printf_exec("parse_and_run_stream: run_and_free_list\n");
		run_and_free_list(pipe_list);
	}
}

static void parse_and_run_string(const char *s)
{
	struct in_str input;
	setup_string_in_str(&input, s);
	parse_and_run_stream(&input, '\0');
}

static void parse_and_run_file(FILE *f)
{
	struct in_str input;
	setup_file_in_str(&input, f);
	parse_and_run_stream(&input, ';');
}

/* Called a few times only (or even once if "sh -c") */
static void block_signals(int second_time)
{
	unsigned sig;
	unsigned mask;

	mask = (1 << SIGQUIT);
	if (G_interactive_fd) {
		mask = 0
			| (1 << SIGQUIT)
			| (1 << SIGTERM)
//TODO			| (1 << SIGHUP)
#if ENABLE_HUSH_JOB
			| (1 << SIGTTIN) | (1 << SIGTTOU) | (1 << SIGTSTP)
#endif
			| (1 << SIGINT)
		;
	}
	G.non_DFL_mask = mask;

	if (!second_time)
		sigprocmask(SIG_SETMASK, NULL, &G.blocked_set);
	sig = 0;
	while (mask) {
		if (mask & 1)
			sigaddset(&G.blocked_set, sig);
		mask >>= 1;
		sig++;
	}
	sigdelset(&G.blocked_set, SIGCHLD);

	sigprocmask(SIG_SETMASK, &G.blocked_set,
			second_time ? NULL : &G.inherited_set);
	/* POSIX allows shell to re-enable SIGCHLD
	 * even if it was SIG_IGN on entry */
//	G.count_SIGCHLD++; /* ensure it is != G.handled_SIGCHLD */
	if (!second_time)
		signal(SIGCHLD, SIG_DFL); // SIGCHLD_handler);
}

#if ENABLE_HUSH_JOB
/* helper */
static void maybe_set_to_sigexit(int sig)
{
	void (*handler)(int);
	/* non_DFL_mask'ed signals are, well, masked,
	 * no need to set handler for them.
	 */
	if (!((G.non_DFL_mask >> sig) & 1)) {
		handler = signal(sig, sigexit);
		if (handler == SIG_IGN) /* oops... restore back to IGN! */
			signal(sig, handler);
	}
}
/* Set handlers to restore tty pgrp and exit */
static void set_fatal_handlers(void)
{
	/* We _must_ restore tty pgrp on fatal signals */
	if (HUSH_DEBUG) {
		maybe_set_to_sigexit(SIGILL );
		maybe_set_to_sigexit(SIGFPE );
		maybe_set_to_sigexit(SIGBUS );
		maybe_set_to_sigexit(SIGSEGV);
		maybe_set_to_sigexit(SIGTRAP);
	} /* else: hush is perfect. what SEGV? */
	maybe_set_to_sigexit(SIGABRT);
	/* bash 3.2 seems to handle these just like 'fatal' ones */
	maybe_set_to_sigexit(SIGPIPE);
	maybe_set_to_sigexit(SIGALRM);
//TODO: disable and move down when proper SIGHUP handling is added
	maybe_set_to_sigexit(SIGHUP );
	/* if we are interactive, [SIGHUP,] SIGTERM and SIGINT are masked.
	 * if we aren't interactive... but in this case
	 * we never want to restore pgrp on exit, and this fn is not called */
	/*maybe_set_to_sigexit(SIGTERM);*/
	/*maybe_set_to_sigexit(SIGINT );*/
}
#endif

static int set_mode(const char cstate, const char mode)
{
	int state = (cstate == '-' ? 1 : 0);
	switch (mode) {
		case 'n': G.fake_mode = state; break;
		case 'x': /*G.debug_mode = state;*/ break;
		default:  return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int hush_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int hush_main(int argc, char **argv)
{
	static const struct variable const_shell_ver = {
		.next = NULL,
		.varstr = (char*)hush_version_str,
		.max_len = 1, /* 0 can provoke free(name) */
		.flg_export = 1,
		.flg_read_only = 1,
	};
	int signal_mask_is_inited = 0;
	int opt;
	char **e;
	struct variable *cur_var;

	INIT_G();
	if (EXIT_SUCCESS) /* if EXIT_SUCCESS == 0, is already done */
		G.last_exitcode = EXIT_SUCCESS;
#if !BB_MMU
	G.argv0_for_re_execing = argv[0];
#endif
	/* Deal with HUSH_VERSION */
	G.shell_ver = const_shell_ver; /* copying struct here */
	G.top_var = &G.shell_ver;
	debug_printf_env("unsetenv '%s'\n", "HUSH_VERSION");
	unsetenv("HUSH_VERSION"); /* in case it exists in initial env */
	/* Initialize our shell local variables with the values
	 * currently living in the environment */
	cur_var = G.top_var;
	e = environ;
	if (e) while (*e) {
		char *value = strchr(*e, '=');
		if (value) { /* paranoia */
			cur_var->next = xzalloc(sizeof(*cur_var));
			cur_var = cur_var->next;
			cur_var->varstr = *e;
			cur_var->max_len = strlen(*e);
			cur_var->flg_export = 1;
		}
		e++;
	}
	debug_printf_env("putenv '%s'\n", hush_version_str);
	putenv((char *)hush_version_str); /* reinstate HUSH_VERSION */
#if ENABLE_FEATURE_EDITING
	G.line_input_state = new_line_input_t(FOR_SHELL);
#endif
	G.global_argc = argc;
	G.global_argv = argv;
	/* Initialize some more globals to non-zero values */
	set_cwd();
#if ENABLE_HUSH_INTERACTIVE
	if (ENABLE_FEATURE_EDITING)
		cmdedit_set_initial_prompt();
	G.PS2 = "> ";
#endif

	/* Shell is non-interactive at first. We need to call
	 * block_signals(0) if we are going to execute "sh <script>",
	 * "sh -c <cmds>" or login shell's /etc/profile and friends.
	 * If we later decide that we are interactive, we run block_signals(0)
	 * (or re-run block_signals(1) if we ran block_signals(0) before)
	 * in order to intercept (more) signals.
	 */

	/* Parse options */
	/* http://www.opengroup.org/onlinepubs/9699919799/utilities/sh.html */
	while (1) {
		opt = getopt(argc, argv, "c:xins"
#if !BB_MMU
				"<:$:!:?:D:R:V:"
#endif
		);
		if (opt <= 0)
			break;
		switch (opt) {
		case 'c':
			if (!G.root_pid)
				G.root_pid = getpid();
			G.global_argv = argv + optind;
			if (!argv[optind]) {
				/* -c 'script' (no params): prevent empty $0 */
				*--G.global_argv = argv[0];
				optind--;
			} /* else -c 'script' PAR0 PAR1: $0 is PAR0 */
			G.global_argc = argc - optind;
			block_signals(0); /* 0: called 1st time */
			parse_and_run_string(optarg);
			goto final_return;
		case 'i':
			/* Well, we cannot just declare interactiveness,
			 * we have to have some stuff (ctty, etc) */
			/* G_interactive_fd++; */
			break;
		case 's':
			/* "-s" means "read from stdin", but this is how we always
			 * operate, so simply do nothing here. */
			break;
#if !BB_MMU
		case '<': /* "big heredoc" support */
			full_write(STDOUT_FILENO, optarg, strlen(optarg));
			_exit(0);
		case '$':
			G.root_pid = bb_strtou(optarg, &optarg, 16);
			optarg++;
			G.last_bg_pid = bb_strtou(optarg, &optarg, 16);
			optarg++;
			G.last_exitcode = bb_strtou(optarg, &optarg, 16);
# if ENABLE_HUSH_LOOPS
			optarg++;
			G.depth_of_loop = bb_strtou(optarg, &optarg, 16);
# endif
			break;
		case 'R':
		case 'V':
			set_local_var(xstrdup(optarg), 0, opt == 'R');
			break;
#endif
		case 'n':
		case 'x':
			if (!set_mode('-', opt))
				break;
		default:
#ifndef BB_VER
			fprintf(stderr, "Usage: sh [FILE]...\n"
					"   or: sh -c command [args]...\n\n");
			exit(EXIT_FAILURE);
#else
			bb_show_usage();
#endif
		}
	} /* option parsing loop */

	if (!G.root_pid)
		G.root_pid = getpid();

	/* If we are login shell... */
	if (argv[0] && argv[0][0] == '-') {
		FILE *input;
		/* XXX what should argv be while sourcing /etc/profile? */
		debug_printf("sourcing /etc/profile\n");
		input = fopen_for_read("/etc/profile");
		if (input != NULL) {
			close_on_exec_on(fileno(input));
			block_signals(0); /* 0: called 1st time */
			signal_mask_is_inited = 1;
			parse_and_run_file(input);
			fclose(input);
		}
		/* bash: after sourcing /etc/profile,
		 * tries to source (in the given order):
		 * ~/.bash_profile, ~/.bash_login, ~/.profile,
		 * stopping of first found. --noprofile turns this off.
		 * bash also sources ~/.bash_logout on exit.
		 * If called as sh, skips .bash_XXX files.
		 */
	}

	if (argv[optind]) {
		FILE *input;
		/*
		 * "bash <script>" (which is never interactive (unless -i?))
		 * sources $BASH_ENV here (without scanning $PATH).
		 * If called as sh, does the same but with $ENV.
		 */
		debug_printf("running script '%s'\n", argv[optind]);
		G.global_argv = argv + optind;
		G.global_argc = argc - optind;
		input = xfopen_for_read(argv[optind]);
		close_on_exec_on(fileno(input));
		if (!signal_mask_is_inited)
			block_signals(0); /* 0: called 1st time */
		parse_and_run_file(input);
#if ENABLE_FEATURE_CLEAN_UP
		fclose(input);
#endif
		goto final_return;
	}

	/* Up to here, shell was non-interactive. Now it may become one.
	 * NB: don't forget to (re)run block_signals(0/1) as needed.
	 */

	/* A shell is interactive if the '-i' flag was given, or if all of
	 * the following conditions are met:
	 *    no -c command
	 *    no arguments remaining or the -s flag given
	 *    standard input is a terminal
	 *    standard output is a terminal
	 * Refer to Posix.2, the description of the 'sh' utility.
	 */
#if ENABLE_HUSH_JOB
	if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
		G.saved_tty_pgrp = tcgetpgrp(STDIN_FILENO);
		debug_printf("saved_tty_pgrp:%d\n", G.saved_tty_pgrp);
//TODO: "interactive" and "have job control" are two different things.
//If tcgetpgrp fails here, "have job control" is false, but "interactive"
//should stay on! Currently, we mix these into one.
		if (G.saved_tty_pgrp >= 0) {
			/* try to dup stdin to high fd#, >= 255 */
			G_interactive_fd = fcntl(STDIN_FILENO, F_DUPFD, 255);
			if (G_interactive_fd < 0) {
				/* try to dup to any fd */
				G_interactive_fd = dup(STDIN_FILENO);
				if (G_interactive_fd < 0)
					/* give up */
					G_interactive_fd = 0;
			}
// TODO: track & disallow any attempts of user
// to (inadvertently) close/redirect it
		}
	}
	debug_printf("interactive_fd:%d\n", G_interactive_fd);
	if (G_interactive_fd) {
		pid_t shell_pgrp;

		/* We are indeed interactive shell, and we will perform
		 * job control. Setting up for that. */

		close_on_exec_on(G_interactive_fd);
		/* If we were run as 'hush &', sleep until we are
		 * in the foreground (tty pgrp == our pgrp).
		 * If we get started under a job aware app (like bash),
		 * make sure we are now in charge so we don't fight over
		 * who gets the foreground */
		while (1) {
			shell_pgrp = getpgrp();
			G.saved_tty_pgrp = tcgetpgrp(G_interactive_fd);
			if (G.saved_tty_pgrp == shell_pgrp)
				break;
			/* send TTIN to ourself (should stop us) */
			kill(- shell_pgrp, SIGTTIN);
		}
		/* Block some signals */
		block_signals(signal_mask_is_inited);
		/* Set other signals to restore saved_tty_pgrp */
		set_fatal_handlers();
		/* Put ourselves in our own process group */
		bb_setpgrp(); /* is the same as setpgid(our_pid, our_pid); */
		/* Grab control of the terminal */
		tcsetpgrp(G_interactive_fd, getpid());
		/* -1 is special - makes xfuncs longjmp, not exit
		 * (we reset die_sleep = 0 whereever we [v]fork) */
		enable_restore_tty_pgrp_on_exit(); /* sets die_sleep = -1 */
		if (setjmp(die_jmp)) {
			/* xfunc has failed! die die die */
			hush_exit(xfunc_error_retval);
		}
	} else if (!signal_mask_is_inited) {
		block_signals(0); /* 0: called 1st time */
	} /* else: block_signals(0) was done before */
#elif ENABLE_HUSH_INTERACTIVE
	/* No job control compiled in, only prompt/line editing */
	if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
		G_interactive_fd = fcntl(STDIN_FILENO, F_DUPFD, 255);
		if (G_interactive_fd < 0) {
			/* try to dup to any fd */
			G_interactive_fd = dup(STDIN_FILENO);
			if (G_interactive_fd < 0)
				/* give up */
				G_interactive_fd = 0;
		}
	}
	if (G_interactive_fd) {
		close_on_exec_on(G_interactive_fd);
		block_signals(signal_mask_is_inited);
	} else if (!signal_mask_is_inited) {
		block_signals(0);
	}
#else
	/* We have interactiveness code disabled */
	if (!signal_mask_is_inited) {
		block_signals(0);
	}
#endif
	/* bash:
	 * if interactive but not a login shell, sources ~/.bashrc
	 * (--norc turns this off, --rcfile <file> overrides)
	 */

	if (!ENABLE_FEATURE_SH_EXTRA_QUIET && G_interactive_fd) {
		printf("\n\n%s hush - the humble shell\n", bb_banner);
		printf("Enter 'help' for a list of built-in commands.\n\n");
	}

	parse_and_run_file(stdin);

 final_return:
#if ENABLE_FEATURE_CLEAN_UP
	if (G.cwd != bb_msg_unknown)
		free((char*)G.cwd);
	cur_var = G.top_var->next;
	while (cur_var) {
		struct variable *tmp = cur_var;
		if (!cur_var->max_len)
			free(cur_var->varstr);
		cur_var = cur_var->next;
		free(tmp);
	}
#endif
	hush_exit(G.last_exitcode);
}


#if ENABLE_LASH
int lash_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int lash_main(int argc, char **argv)
{
	//bb_error_msg("lash is deprecated, please use hush instead");
	return hush_main(argc, argv);
}
#endif


/*
 * Built-ins
 */
static int builtin_trap(char **argv)
{
	int i;
	int sig;
	char *new_cmd;

	if (!G.traps)
		G.traps = xzalloc(sizeof(G.traps[0]) * NSIG);

	argv++;
	if (!*argv) {
		/* No args: print all trapped. This isn't 100% correct as we
		 * should be escaping the cmd so that it can be pasted back in
		 */
		for (i = 0; i < NSIG; ++i)
			if (G.traps[i])
				printf("trap -- '%s' %s\n", G.traps[i], get_signame(i));
		return EXIT_SUCCESS;
	}

	new_cmd = NULL;
	i = 0;
	/* If first arg is decimal: reset all specified signals */
	sig = bb_strtou(*argv, NULL, 10);
	if (errno == 0) {
		int ret;
 set_all:
		ret = EXIT_SUCCESS;
		while (*argv) {
			sig = get_signum(*argv++);
			if (sig < 0 || sig >= NSIG) {
				ret = EXIT_FAILURE;
				/* Mimic bash message exactly */
				bb_perror_msg("trap: %s: invalid signal specification", argv[i]);
				continue;
			}

			free(G.traps[sig]);
			G.traps[sig] = xstrdup(new_cmd);

			debug_printf("trap: setting SIG%s (%i) to '%s'",
				get_signame(sig), sig, G.traps[sig]);

			/* There is no signal for 0 (EXIT) */
			if (sig == 0)
				continue;

			if (new_cmd) {
				sigaddset(&G.blocked_set, sig);
			} else {
				/* There was a trap handler, we are removing it
				 * (if sig has non-DFL handling,
				 * we don't need to do anything) */
				if (sig < 32 && (G.non_DFL_mask & (1 << sig)))
					continue;
				sigdelset(&G.blocked_set, sig);
			}
			sigprocmask(SIG_SETMASK, &G.blocked_set, NULL);
		}
		return ret;
	}

	/* First arg is "-": reset all specified to default */
	/* First arg is "": ignore all specified */
	/* Everything else: execute first arg upon signal */
	if (!argv[1]) {
		bb_error_msg("trap: invalid arguments");
		return EXIT_FAILURE;
	}
	if (NOT_LONE_DASH(*argv))
		new_cmd = *argv;
	argv++;
	goto set_all;
}

static int builtin_true(char **argv UNUSED_PARAM)
{
	return 0;
}

static int builtin_test(char **argv)
{
	int argc = 0;
	while (*argv) {
		argc++;
		argv++;
	}
	return test_main(argc, argv - argc);
}

static int builtin_echo(char **argv)
{
	int argc = 0;
	while (*argv) {
		argc++;
		argv++;
	}
	return echo_main(argc, argv - argc);
}

static int builtin_eval(char **argv)
{
	int rcode = EXIT_SUCCESS;

	if (*++argv) {
		char *str = expand_strvec_to_string(argv);
		/* bash:
		 * eval "echo Hi; done" ("done" is syntax error):
		 * "echo Hi" will not execute too.
		 */
		parse_and_run_string(str);
		free(str);
		rcode = G.last_exitcode;
	}
	return rcode;
}

static int builtin_cd(char **argv)
{
	const char *newdir = argv[1];
	if (newdir == NULL) {
		/* bash does nothing (exitcode 0) if HOME is ""; if it's unset,
		 * bash says "bash: cd: HOME not set" and does nothing (exitcode 1)
		 */
		newdir = getenv("HOME") ? : "/";
	}
	if (chdir(newdir)) {
		/* Mimic bash message exactly */
		bb_perror_msg("cd: %s", newdir);
		return EXIT_FAILURE;
	}
	set_cwd();
	return EXIT_SUCCESS;
}

static int builtin_exec(char **argv)
{
	if (*++argv == NULL)
		return EXIT_SUCCESS; /* bash does this */
	{
#if !BB_MMU
		nommu_save_t dummy;
#endif
// FIXME: if exec fails, bash does NOT exit! We do...
		pseudo_exec_argv(&dummy, argv, 0, NULL);
		/* never returns */
	}
}

static int builtin_exit(char **argv)
{
	debug_printf_exec("%s()\n", __func__);
// TODO: bash does it ONLY on top-level sh exit (+interacive only?)
	//puts("exit"); /* bash does it */
// TODO: warn if we have background jobs: "There are stopped jobs"
// On second consecutive 'exit', exit anyway.
	if (*++argv == NULL)
		hush_exit(G.last_exitcode);
	/* mimic bash: exit 123abc == exit 255 + error msg */
	xfunc_error_retval = 255;
	/* bash: exit -2 == exit 254, no error msg */
	hush_exit(xatoi(*argv) & 0xff);
}

static int builtin_export(char **argv)
{
	if (*++argv == NULL) {
		// TODO:
		// ash emits: export VAR='VAL'
		// bash: declare -x VAR="VAL"
		// (both also escape as needed (quotes, $, etc))
		char **e = environ;
		if (e)
			while (*e)
				puts(*e++);
		return EXIT_SUCCESS;
	}

	do {
		const char *value;
		char *name = *argv;

		value = strchr(name, '=');
		if (!value) {
			/* They are exporting something without a =VALUE */
			struct variable *var;

			var = get_local_var(name);
			if (var) {
				var->flg_export = 1;
				debug_printf_env("%s: putenv '%s'\n", __func__, var->varstr);
				putenv(var->varstr);
			}
			/* bash does not return an error when trying to export
			 * an undefined variable.  Do likewise. */
			continue;
		}
		set_local_var(xstrdup(name), 1, 0);
	} while (*++argv);

	return EXIT_SUCCESS;
}

#if ENABLE_HUSH_JOB
/* built-in 'fg' and 'bg' handler */
static int builtin_fg_bg(char **argv)
{
	int i, jobnum;
	struct pipe *pi;

	if (!G_interactive_fd)
		return EXIT_FAILURE;
	/* If they gave us no args, assume they want the last backgrounded task */
	if (!argv[1]) {
		for (pi = G.job_list; pi; pi = pi->next) {
			if (pi->jobid == G.last_jobid) {
				goto found;
			}
		}
		bb_error_msg("%s: no current job", argv[0]);
		return EXIT_FAILURE;
	}
	if (sscanf(argv[1], "%%%d", &jobnum) != 1) {
		bb_error_msg("%s: bad argument '%s'", argv[0], argv[1]);
		return EXIT_FAILURE;
	}
	for (pi = G.job_list; pi; pi = pi->next) {
		if (pi->jobid == jobnum) {
			goto found;
		}
	}
	bb_error_msg("%s: %d: no such job", argv[0], jobnum);
	return EXIT_FAILURE;
 found:
	// TODO: bash prints a string representation
	// of job being foregrounded (like "sleep 1 | cat")
	if (argv[0][0] == 'f') {
		/* Put the job into the foreground.  */
		tcsetpgrp(G_interactive_fd, pi->pgrp);
	}

	/* Restart the processes in the job */
	debug_printf_jobs("reviving %d procs, pgrp %d\n", pi->num_cmds, pi->pgrp);
	for (i = 0; i < pi->num_cmds; i++) {
		debug_printf_jobs("reviving pid %d\n", pi->cmds[i].pid);
		pi->cmds[i].is_stopped = 0;
	}
	pi->stopped_cmds = 0;

	i = kill(- pi->pgrp, SIGCONT);
	if (i < 0) {
		if (errno == ESRCH) {
			delete_finished_bg_job(pi);
			return EXIT_SUCCESS;
		}
		bb_perror_msg("kill (SIGCONT)");
	}

	if (argv[0][0] == 'f') {
		remove_bg_job(pi);
		return checkjobs_and_fg_shell(pi);
	}
	return EXIT_SUCCESS;
}
#endif

#if ENABLE_HUSH_HELP
static int builtin_help(char **argv UNUSED_PARAM)
{
	const struct built_in_command *x;

	printf("\n"
		"Built-in commands:\n"
		"------------------\n");
	for (x = bltins; x != &bltins[ARRAY_SIZE(bltins)]; x++) {
		printf("%s\t%s\n", x->cmd, x->descr);
	}
	printf("\n\n");
	return EXIT_SUCCESS;
}
#endif

#if ENABLE_HUSH_JOB
static int builtin_jobs(char **argv UNUSED_PARAM)
{
	struct pipe *job;
	const char *status_string;

	for (job = G.job_list; job; job = job->next) {
		if (job->alive_cmds == job->stopped_cmds)
			status_string = "Stopped";
		else
			status_string = "Running";

		printf(JOB_STATUS_FORMAT, job->jobid, status_string, job->cmdtext);
	}
	return EXIT_SUCCESS;
}
#endif

#if HUSH_DEBUG
static int builtin_memleak(char **argv UNUSED_PARAM)
{
	void *p;
	unsigned long l;

	/* Crude attempt to find where "free memory" starts,
	 * sans fragmentation. */
	p = malloc(240);
	l = (unsigned long)p;
	free(p);
	p = malloc(3400);
	if (l < (unsigned long)p) l = (unsigned long)p;
	free(p);

	if (!G.memleak_value)
		G.memleak_value = l;
	
	l -= G.memleak_value;
	if ((long)l < 0)
		l = 0;
	l /= 1024;
	if (l > 127)
		l = 127;

	/* Exitcode is "how many kilobytes we leaked since 1st call" */
	return l;
}
#endif

static int builtin_pwd(char **argv UNUSED_PARAM)
{
	puts(set_cwd());
	return EXIT_SUCCESS;
}

static int builtin_read(char **argv)
{
	char *string;
	const char *name = argv[1] ? argv[1] : "REPLY";
//TODO: check that argv[1] is a valid variable name

	string = xmalloc_reads(STDIN_FILENO, xasprintf("%s=", name), NULL);
	return set_local_var(string, 0, 0);
}

/* http://www.opengroup.org/onlinepubs/9699919799/utilities/V3_chap02.html#set
 * built-in 'set' handler
 * SUSv3 says:
 * set [-abCefhmnuvx] [-o option] [argument...]
 * set [+abCefhmnuvx] [+o option] [argument...]
 * set -- [argument...]
 * set -o
 * set +o
 * Implementations shall support the options in both their hyphen and
 * plus-sign forms. These options can also be specified as options to sh.
 * Examples:
 * Write out all variables and their values: set
 * Set $1, $2, and $3 and set "$#" to 3: set c a b
 * Turn on the -x and -v options: set -xv
 * Unset all positional parameters: set --
 * Set $1 to the value of x, even if it begins with '-' or '+': set -- "$x"
 * Set the positional parameters to the expansion of x, even if x expands
 * with a leading '-' or '+': set -- $x
 *
 * So far, we only support "set -- [argument...]" and some of the short names.
 */
static int builtin_set(char **argv)
{
	int n;
	char **pp, **g_argv;
	char *arg = *++argv;

	if (arg == NULL) {
		struct variable *e;
		for (e = G.top_var; e; e = e->next)
			puts(e->varstr);
		return EXIT_SUCCESS;
	}

	do {
		if (!strcmp(arg, "--")) {
			++argv;
			goto set_argv;
		}

		if (arg[0] == '+' || arg[0] == '-') {
			for (n = 1; arg[n]; ++n)
				if (set_mode(arg[0], arg[n]))
					goto error;
			continue;
		}

		break;
	} while ((arg = *++argv) != NULL);
	/* Now argv[0] is 1st argument */

	/* Only reset global_argv if we didn't process anything */
	if (arg == NULL)
		return EXIT_SUCCESS;
 set_argv:

	/* NB: G.global_argv[0] ($0) is never freed/changed */
	g_argv = G.global_argv;
	if (G.global_args_malloced) {
		pp = g_argv;
		while (*++pp)
			free(*pp);
		g_argv[1] = NULL;
	} else {
		G.global_args_malloced = 1;
		pp = xzalloc(sizeof(pp[0]) * 2);
		pp[0] = g_argv[0]; /* retain $0 */
		g_argv = pp;
	}
	/* This realloc's G.global_argv */
	G.global_argv = pp = add_strings_to_strings(g_argv, argv, /*dup:*/ 1);

	n = 1;
	while (*++pp)
		n++;
	G.global_argc = n;

	return EXIT_SUCCESS;

	/* Nothing known, so abort */
 error:
	bb_error_msg("set: %s: invalid option", arg);
	return EXIT_FAILURE;
}

static int builtin_shift(char **argv)
{
	int n = 1;
	if (argv[1]) {
		n = atoi(argv[1]);
	}
	if (n >= 0 && n < G.global_argc) {
		if (G.global_args_malloced) {
			int m = 1;
			while (m <= n)
				free(G.global_argv[m++]);
		}
		G.global_argc -= n;
		memmove(&G.global_argv[1], &G.global_argv[n+1],
				G.global_argc * sizeof(G.global_argv[0]));
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}

static int builtin_source(char **argv)
{
	FILE *input;

	if (*++argv == NULL)
		return EXIT_FAILURE;

	/* XXX search through $PATH is missing */
	input = fopen_or_warn(*argv, "r");
	if (!input) {
		/* bb_perror_msg("%s", *argv); - done by fopen_or_warn */
		return EXIT_FAILURE;
	}
	close_on_exec_on(fileno(input));

	/* Now run the file */
//TODO:
	/* XXX argv and argc are broken; need to save old G.global_argv
	 * (pointer only is OK!) on this stack frame,
	 * set G.global_argv=argv+1, recurse, and restore. */
	parse_and_run_file(input);
	fclose(input);
	return G.last_exitcode;
}

static int builtin_umask(char **argv)
{
	mode_t new_umask;
	const char *arg = argv[1];
	if (arg) {
//TODO: umask may take chmod-like symbolic masks
		new_umask = bb_strtou(arg, NULL, 8);
		if (errno) {
			//Message? bash examples:
			//bash: umask: 'q': invalid symbolic mode operator
			//bash: umask: 999: octal number out of range
			return EXIT_FAILURE;
		}
	} else {
		new_umask = umask(0);
		printf("%.3o\n", (unsigned) new_umask);
		/* fall through and restore new_umask which we set to 0 */
	}
	umask(new_umask);
	return EXIT_SUCCESS;
}

/* http://www.opengroup.org/onlinepubs/9699919799/utilities/V3_chap02.html#unset */
static int builtin_unset(char **argv)
{
	int ret;
	char var;

	if (!*++argv)
		return EXIT_SUCCESS;

	var = 'v';
	if (argv[0][0] == '-') {
		switch (argv[0][1]) {
		case 'v':
		case 'f':
			var = argv[0][1];
			break;
		default:
			bb_error_msg("unset: %s: invalid option", *argv);
			return EXIT_FAILURE;
		}
//TODO: disallow "unset -vf ..." too
		argv++;
	}

	ret = EXIT_SUCCESS;
	while (*argv) {
		if (var == 'v') {
			if (unset_local_var(*argv)) {
				/* unset <nonexistent_var> doesn't fail.
				 * Error is when one tries to unset RO var.
				 * Message was printed by unset_local_var. */
				ret = EXIT_FAILURE;
			}
		}
#if ENABLE_HUSH_FUNCTIONS
		else {
			unset_local_func(*argv);
		}
#endif
		argv++;
	}
	return ret;
}

/* http://www.opengroup.org/onlinepubs/9699919799/utilities/wait.html */
static int builtin_wait(char **argv)
{
	int ret = EXIT_SUCCESS;
	int status, sig;

	if (*++argv == NULL) {
		/* Don't care about wait results */
		/* Note 1: must wait until there are no more children */
		/* Note 2: must be interruptible */
		/* Examples:
		 * $ sleep 3 & sleep 6 & wait
		 * [1] 30934 sleep 3
		 * [2] 30935 sleep 6
		 * [1] Done                   sleep 3
		 * [2] Done                   sleep 6
		 * $ sleep 3 & sleep 6 & wait
		 * [1] 30936 sleep 3
		 * [2] 30937 sleep 6
		 * [1] Done                   sleep 3
		 * ^C <-- after ~4 sec from keyboard
		 * $
		 */
		sigaddset(&G.blocked_set, SIGCHLD);
		sigprocmask(SIG_SETMASK, &G.blocked_set, NULL);
		while (1) {
			checkjobs(NULL);
			if (errno == ECHILD)
				break;
			/* Wait for SIGCHLD or any other signal of interest */
			/* sigtimedwait with infinite timeout: */
			sig = sigwaitinfo(&G.blocked_set, NULL);
			if (sig > 0) {
				sig = check_and_run_traps(sig);
				if (sig && sig != SIGCHLD) { /* see note 2 */
					ret = 128 + sig;
					break;
				}
			}
		}
		sigdelset(&G.blocked_set, SIGCHLD);
		sigprocmask(SIG_SETMASK, &G.blocked_set, NULL);
		return ret;
	}

	/* This is probably buggy wrt interruptible-ness */
	while (*argv) {
		pid_t pid = bb_strtou(*argv, NULL, 10);
		if (errno) {
			/* mimic bash message */
			bb_error_msg("wait: '%s': not a pid or valid job spec", *argv);
			return EXIT_FAILURE;
		}
		if (waitpid(pid, &status, 0) == pid) {
			if (WIFSIGNALED(status))
				ret = 128 + WTERMSIG(status);
			else if (WIFEXITED(status))
				ret = WEXITSTATUS(status);
			else /* wtf? */
				ret = EXIT_FAILURE;
		} else {
			bb_perror_msg("wait %s", *argv);
			ret = 127;
		}
		argv++;
	}

	return ret;
}

#if ENABLE_HUSH_LOOPS
static int builtin_break(char **argv)
{
	if (G.depth_of_loop == 0) {
		bb_error_msg("%s: only meaningful in a loop", argv[0]);
		return EXIT_SUCCESS; /* bash compat */
	}
	G.flag_break_continue++; /* BC_BREAK = 1 */
	G.depth_break_continue = 1;
	if (argv[1]) {
		G.depth_break_continue = bb_strtou(argv[1], NULL, 10);
		if (errno || !G.depth_break_continue || argv[2]) {
			bb_error_msg("%s: bad arguments", argv[0]);
			G.flag_break_continue = BC_BREAK;
			G.depth_break_continue = UINT_MAX;
		}
	}
	if (G.depth_of_loop < G.depth_break_continue)
		G.depth_break_continue = G.depth_of_loop;
	return EXIT_SUCCESS;
}

static int builtin_continue(char **argv)
{
	G.flag_break_continue = 1; /* BC_CONTINUE = 2 = 1+1 */
	return builtin_break(argv);
}
#endif
