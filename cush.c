/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <spawn.h>
#include <fcntl.h>
#include <readline/history.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"

static void handle_child_status(pid_t pid, int status);

void run_command(struct ast_command_line *command_line);

int is_builtin(char **cmd);

void jobs(void);

void clean_joblist(void);

struct job *find_job(pid_t pid);

extern char **environ;

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
           " -h            print this help\n",
           progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status
{
    FOREGROUND,    /* job is running in foreground.  Only one job can be
                      in the foreground state. */
    BACKGROUND,    /* job is running in background */
    STOPPED,       /* job is stopped via SIGSTOP */
    NEEDSTERMINAL, /* job is stopped because it was a background job
                      and requires exclusive terminal access */
};

struct job
{
    struct list_elem elem;          /* Link element for jobs list. */
    struct ast_pipeline *pipe;      /* The pipeline of commands this job represents */
    int jid;                        /* Job id. */
    enum job_status status;         /* Job status. */
    int num_processes_alive;        /* The number of processes that we know to be alive */
    struct termios saved_tty_state; /* The state of the terminal when this job was
                                       stopped after having been in foreground */
    pid_t pgid; /*The process group id*/
    pid_t pid_list[20]; /*Array of child process IDs*/
    bool saved_state_changed; /*This indicate if saved_tty_state was changed or not*/
    int num_pids; /*Number of process Id that was created*/
};

/* Utility functions for job list management.
 * We use 2 data structures:
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1 << 16)
static struct list job_list;
static struct job *jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job *
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job *job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++)
    {
        if (jid2job[i] == NULL)
        {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    ast_pipeline_free(job->pipe);
    free(job);
}

/* Get a current status of a job */
static const char *
get_status(enum job_status status)
{
    switch (status)
    {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem *e = list_begin(&pipeline->commands);
    for (; e != list_end(&pipeline->commands); e = list_next(e))
    {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/* Print a job */
static void
print_job(struct job *job)
{
    printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
    print_cmdline(job->pipe);
    printf(")\n");
}

/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD
 * signal may be delivered for multiple children that have
 * exited. All of them need to be reaped.
 */
static void sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0)
    {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 *
 * Implement handle_child_status such that it records the
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0)
    {

        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }
}

/* With the given pid and status determine which job is this pid part of and determine what
    satuts change occurred using the WIF() macros. Then, undate the job status accordingly,
    and adjust num_process alive if process died. If a process was stopped, save the terminal state.*/
static void handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));
    /* interate through the job list */
    for (struct list_elem *e = list_begin(&job_list);
         e != list_end(&job_list);
         e = list_next(e))
    {
        struct job *job1 = list_entry(e, struct job, elem);
        job1->saved_state_changed = false;
        /* for each job -> pid_list and compare every one of them */
        for (int i = 0; i < job1->num_pids; i++)
        {
            /* If found the job that has the given pid part of it*/
            if ((job1->pid_list[i]) == pid)
            {
                /* Process stopped not dead
                 so no decrement in num process alive in this job 
                 but change the status of current job */
                if (WIFSTOPPED(status))
                {
                    // User stops FOREGROUND process with Ctrl-Z
                    if (WSTOPSIG(status) == SIGTSTP)
                    {
                        job1->status = STOPPED;
                        print_job(job1);
                        termstate_save(&job1->saved_tty_state);
                        job1->saved_state_changed = true;
                    }
                    // User stops process with kill -STOP
                    else if (WSTOPSIG(status) == SIGSTOP)
                    {
                        job1->status = STOPPED;
                        termstate_save(&job1->saved_tty_state);
                        job1->saved_state_changed = true;
                    }
                    // non-foreground processs wants terminal access
                    else if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN)
                    {
                        job1->status = NEEDSTERMINAL;
                        termstate_save(&job1->saved_tty_state);
                        job1->saved_state_changed = true;
                    }
                }
                /* When process terminated regularly (General case)
                    decrement the number of process alive*/
                else if (WIFEXITED(status))
                {
                    /* If job is in FOREGRONG sample the last know good state
                        of terminal*/
                    if (job1->status == FOREGROUND)
                    {
                        termstate_sample();
                    }
                    job1->num_processes_alive--;
                }
                /*  When process get specific signal and terminated manually
                    Decrement the number of process alive */
                else if (WIFSIGNALED(status))
                {
                    if (WTERMSIG(status) == SIGFPE)
                    {
                        printf("floating point exception\n");
                    }
                    if (WTERMSIG(status) == SIGSEGV)
                    {
                        printf("segmentation fault\n");
                    }
                    if (WTERMSIG(status) == SIGABRT)
                    {
                        printf("aborted\n");
                    }
                    if (WTERMSIG(status) == SIGKILL)
                    {
                        printf("killed\n");
                    }
                    if (WTERMSIG(status) == SIGTERM)
                    {
                        printf("terminated\n");
                    }
                    job1->num_processes_alive--;
                }
            }
        }
    }
}

int main(int ac, char *av[])
{
    int opt;
    // Sets up history
    using_history();

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0)
    {
        switch (opt)
        {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init();

    /* Read/eval loop. */
    for (;;)
    {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char *prompt = isatty(0) ? build_prompt() : NULL;
        char *cmdline = readline(prompt);
        free(prompt);

        clean_joblist();

        if (cmdline == NULL) /* User typed EOF */
            break;

        // Checks for event discriptors for history and adds command to history
        char *eventCheck;
        history_expand(cmdline, &eventCheck);
        if (strstr(cmdline, "!") || strstr(cmdline, "^"))
        {
            add_history(eventCheck);
            cmdline = eventCheck;
        }
        else
        {
            add_history(cmdline);
        }

        struct ast_command_line *cline = ast_parse_command_line(cmdline);
        free(cmdline);
        if (cline == NULL) /* Error in command line */
            continue;

        if (list_empty(&cline->pipes))
        { /* User hit enter */
            ast_command_line_free(cline);
            continue;
        }

        run_command(cline);

        // ast_command_line_print(cline);      /* Output a representation of
        //  the entered command line */

        /* Free the command line.
         * This will free the ast_pipeline objects still contained
         * in the ast_command_line.  Once you implement a job list
         * that may take ownership of ast_pipeline objects that are
         * associated with jobs you will need to reconsider how you
         * manage the lifetime of the associated ast_pipelines.
         * Otherwise, freeing here will cause use-after-free errors.
         */
        // ast_command_line_free(cline);
    }
    return 0;
}

/* Based on the parsing that was handled in main, this function run commands.
    First for loop go over all the pipe lines in command line.
        after that this function determine if the pipe is built in or not

        If built ins, built in functions are runned

        If pipe is not built in job is added to job list
            Second for loop goes over all the commands in pipe line
                for each command we check if input or output need to be redircted
                and create pipeline if there are more than one commands in pipe
                Process group is created before creating the child process
                Close all the pipes if pipes were created
                Wait for all processes in this job to complete, or for
                    the job no longer to be in the foreground.
                After all this give terminal back to shell
*/
void run_command(struct ast_command_line *command_line)
{
    for (struct list_elem *e = list_begin(&command_line->pipes);
         e != list_end(&command_line->pipes);
         e = list_next(e))
    {
        struct ast_pipeline *pipe1 = list_entry(e, struct ast_pipeline, elem);

        struct list_elem *e = list_begin(&pipe1->commands);
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        char **p = cmd->argv;
        struct job *job1 = NULL;
        int builtint = is_builtin(p);
        int count = 0;
        int size1 = list_size(&pipe1->commands);
        int fd[999][2];

        if (builtint == 1)
        {
            job1 = add_job(pipe1);
            for (struct list_elem *e = list_begin(&pipe1->commands);
                 e != list_end(&pipe1->commands);
                 e = list_next(e))
            {
                posix_spawn_file_actions_t file_action;
                posix_spawnattr_t posix_attr;
                posix_spawnattr_init(&posix_attr);
                posix_spawn_file_actions_init(&file_action);

                // If not null last command should write to file iored_outputs
                if (pipe1->iored_input)
                {
                    posix_spawn_file_actions_addopen(&file_action, STDIN_FILENO, pipe1->iored_input, O_RDONLY, 0);
                }
                // If not null first command should read to file iored_input
                if (pipe1->iored_output)
                {
                    if (pipe1->append_to_output)
                                                                                                                                                                                               {
                        posix_spawn_file_actions_addopen(&file_action, STDOUT_FILENO, pipe1->iored_output, O_WRONLY | O_CREAT | O_APPEND, 0644);
                        posix_spawn_file_actions_addopen(&file_action, STDERR_FILENO, pipe1->iored_output, O_WRONLY | O_CREAT | O_APPEND, 0644);
                    }
                    else
                    {
                        posix_spawn_file_actions_addopen(&file_action, STDOUT_FILENO, pipe1->iored_output, O_CREAT | O_RDWR, 0666);
                    }
                    if (cmd->dup_stderr_to_stdout)
                    {
                        posix_spawn_file_actions_adddup2(&file_action, STDOUT_FILENO, STDERR_FILENO);
                    }
                }

                if (pipe1->bg_job)
                {
                    job1->status = BACKGROUND;
                }
                else
                {
                    job1->status = FOREGROUND;
                }

                if (size1 > 1)
                {
                    // First command
                    if (count == 0)
                    {
                        pipe2(fd[count], __O_CLOEXEC);
                        posix_spawn_file_actions_adddup2(&file_action, fd[count][1], STDOUT_FILENO);
                    }
                    // Middle command
                    else if (count != 0 && count != (size1 - 1))
                    {
                        pipe2(fd[count], __O_CLOEXEC);
                        posix_spawn_file_actions_adddup2(&file_action, fd[count - 1][0], STDIN_FILENO);
                        posix_spawn_file_actions_adddup2(&file_action, fd[count][1], STDOUT_FILENO);
                    }
                    // Last command
                    else
                    {
                        posix_spawn_file_actions_adddup2(&file_action, fd[count - 1][0], STDIN_FILENO);
                    }
                }
                count++;
                struct ast_command *cmd = list_entry(e, struct ast_command, elem);
                char **p = cmd->argv;

                if (count == 1)
                {
                    if (job1->status == FOREGROUND)
                    {
                        posix_spawnattr_setflags(&posix_attr, POSIX_SPAWN_TCSETPGROUP | POSIX_SPAWN_SETPGROUP);
                        int fd = termstate_get_tty_fd();
                        posix_spawnattr_tcsetpgrp_np(&posix_attr, fd);
                    }
                    else
                    {
                        posix_spawnattr_setflags(&posix_attr, POSIX_SPAWN_SETPGROUP);
                        posix_spawnattr_setpgroup(&posix_attr, 0);
                    }
                }
                else
                {
                    posix_spawnattr_setflags(&posix_attr, POSIX_SPAWN_SETPGROUP);
                    posix_spawnattr_setpgroup(&posix_attr, job1->pgid);
                }

                if (cmd->dup_stderr_to_stdout)
                {
                    posix_spawn_file_actions_adddup2(&file_action, STDOUT_FILENO, STDERR_FILENO);
                    printf("  stderr shall also be redirected\n");
                }

                if (posix_spawnp(&job1->pid_list[count - 1], p[0], &file_action, &posix_attr, p, environ) == 0)
                {
                    if (job1->status == BACKGROUND && job1->num_processes_alive == 0)
                    {
                        printf("[%d] %d\n", job1->jid, job1->pid_list[0]);
                    }
                    job1->num_pids++;
                    job1->num_processes_alive++;
                }
                else
                {
                    printf("no such file or directory\n");
                }
                if (job1->num_processes_alive == 1)
                {
                    job1->pgid = job1->pid_list[0];
                }
            }
            if (size1 > 1)
            {
                // close all pipe
                for (int i = 0; i < (list_size(&pipe1->commands) - 1); i++)
                {
                    for (int j = 0; j < 2; j++)
                    {
                        close(fd[i][j]);
                    }
                }
            }
            signal_block(SIGCHLD);
            wait_for_job(job1);
            signal_unblock(SIGCHLD);
            termstate_give_terminal_back_to_shell();
        }
    }
}

/* 
    This function determines if command line is a built in or not
    if command line is a built in, corresponding functionality was implemented.
        Return 0 if built in
        Return 1 if not built in
 */
int is_builtin(char **cmd)
{
    if (strcmp(cmd[0], "kill") == 0)
    {

        int jid = atoi(cmd[1]);
        struct job *job1 = get_job_from_jid(jid);

        if (job1 == NULL)
        {
            printf("the process was not killed \n");
        }
        else
        {
            killpg(job1->pgid, SIGTERM);
        }
        return 0;
    }
    else if (strcmp(cmd[0], "fg") == 0)
    {
        int jid = atoi(cmd[1]);
        struct job *job1 = get_job_from_jid(jid);
        print_cmdline(job1->pipe);
        printf("\n");
        job1->status = FOREGROUND;
        if (job1->saved_state_changed == false)
        {
            termstate_give_terminal_to(NULL, job1->pgid);
        }
        else
        {
            termstate_give_terminal_to(&job1->saved_tty_state, job1->pgid);
        }
        killpg(job1->pgid, SIGCONT);
        signal_block(SIGCHLD);
        wait_for_job(job1);
        signal_unblock(SIGCHLD);
        termstate_give_terminal_back_to_shell();
        return 0;
    }
    else if (strcmp(cmd[0], "bg") == 0)
    {

        int jid = atoi(cmd[1]);
        struct job *job1 = get_job_from_jid(jid);

        job1->status = BACKGROUND;
        killpg(job1->pgid, SIGCONT);

        return 0;
    }
    else if (strcmp(cmd[0], "stop") == 0)
    {

        int jid = atoi(cmd[1]);
        struct job *job1 = get_job_from_jid(jid);
        killpg(job1->pgid, SIGSTOP);
        return 0;
    }
    else if (strcmp(cmd[0], "jobs") == 0)
    {
        jobs();
        return 0;
    }
    else if (strcmp(cmd[0], "exit") == 0)
    {
        exit(0);
        return 0;
    }
    else if (strcmp(cmd[0], "cd") == 0)
    {
        if (cmd[1] == NULL)
        {
            chdir(getenv("HOME"));
        }
        else if (chdir(cmd[1]) == -1)
        {
            printf("Failed to change directory\n");
        }
        return 0;
    }
    else if (strcmp(cmd[0], "history") == 0)
    {
        HISTORY_STATE *state = history_get_history_state();

        // Print all of history entries to terminal
        for (int idx = 0; idx < state->length; idx++)
        {
            printf("%d %s \n", idx + 1, state->entries[idx]->line);
        }
        return 0;
    }

    return 1;
}

/*
    This function handles the "jobs" built int
*/
void jobs()
{
    for (struct list_elem *e = list_begin(&job_list);
         e != list_end(&job_list);
         e = list_next(e))
    {
       scl enable gcc-toolset-12 bash struct job *jobber = list_entry(e, struct job, elem);
        print_job(jobber);
    }
}

/*
    This function finds the corresponding job based on given pid
*/
struct job *find_job(pid_t pid)
{
    for (struct list_elem *e = list_begin(&job_list);
         e != list_end(&job_list);
         e = list_next(e))
    {
        struct job *job1 = list_entry(e, struct job, elem);
        for (int i = 0; i < sizeof(job1->pid_list) / sizeof(pid); i++)
        {
            if ((job1->pid_list[i]) == pid)
            {
                return job1;
            }
        }
    }
    return NULL;
}

/*
    This goes over job list and delete all the jobs that has no process alive (terminated)
*/
void clean_joblist()
{
    struct job *arr[MAXJOBS];
    int count = 0;
    for (struct list_elem *e = list_begin(&job_list);
         e != list_end(&job_list);
         e = list_next(e))
    {
        struct job *jobber = list_entry(e, struct job, elem);

        if (jobber->num_processes_alive == 0)
        {
            arr[count] = jobber;
            count++;
        }
    }
    for (int idx = 0; idx < count; idx++)
    {
        list_remove(&arr[idx]->pipe->elem);
        list_remove(&arr[idx]->elem);
        delete_job(arr[idx]);
    }
}
