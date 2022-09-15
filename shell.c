/**
 * @file tsh.c
 * @brief A tiny shell program with job control
 *  Builtin Command:
 *  fg job, bg job, quit, jobs
 *  Builtin command is evaluated by builtincmd() function
 *
 *  This file implements a tiny shell that can respond to builtin job
 *  commands, process foreground job and background jobs by managing
 *  such jobs through job lists.
 *
 *  The eval() function responds to builtin command, creates child processes
 *  to run the current foreground job. Once the foreground job is finished,
 *  the parent process reaps the child and return to accept the next command.
 *  Background jobs can run concurrently and parent process can return
 *  before the background job completes.
 *
 *  This file also implements sigchld_handler, sigint_handler, and
 *  sigtstp_handler to respond to the three signals so it prints the
 *  corresponding message and also reaps children correctly.
 *
 * @author Jiayi Wang
 */

#include "csapp.h"
#include "tsh_helper.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

/* Function prototypes */
void eval(const char *cmdline);
void builtincmd(parseline_return parse_result, struct cmdline_tokens token);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);
void cleanup(void);

/**
 * @brief Runs the shell and accepts command line arguments for shell to eval
 *
 * @return This function does not return because control never reaches the
 * end
 * @param[in] argc Number of arguments in the command line
 * @param[in] argv Array of char pointers that stores the command arguments
 *
 * This function parses the command line and sets up the correct enviroment
 * for shell to run. The function calls the other function eval() to run
 * the child process and executes the command line.
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE_TSH]; // Cmdline for fgets
    bool emit_prompt = true;   // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
        perror("dup2 error");
        exit(1);
    }

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h': // Prints help message
            usage();
            break;
        case 'v': // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p': // Disables prompt printing
            emit_prompt = false;
            break;
        default:
            usage();
        }
    }

    // Create environment variable
    if (putenv("MY_ENV=42") < 0) {
        perror("putenv error");
        exit(1);
    }

    // Set buffering mode of stdout to line buffering.
    // This prevents lines from being printed in the wrong order.
    if (setvbuf(stdout, NULL, _IOLBF, 0) < 0) {
        perror("setvbuf error");
        exit(1);
    }

    // Initialize the job list
    init_job_list();

    // Register a function to clean up the job list on program termination.
    // The function may not run in the case of abnormal termination (e.g. when
    // using exit or terminating due to a signal handler), so in those cases,
    // we trust that the OS will clean up any remaining resources.
    if (atexit(cleanup) < 0) {
        perror("atexit error");
        exit(1);
    }

    // Install the signal handlers
    Signal(SIGINT, sigint_handler);   // Handles Ctrl-C
    Signal(SIGTSTP, sigtstp_handler); // Handles Ctrl-Z
    Signal(SIGCHLD, sigchld_handler); // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler);

    // Execute the shell's read/eval loop
    while (true) {
        if (emit_prompt) {
            printf("%s", prompt);

            // We must flush stdout since we are not printing a full line.
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin)) {
            perror("fgets error");
            exit(1);
        }

        if (feof(stdin)) {
            // End of file (Ctrl-D)
            printf("\n");
            return 0;
        }

        // Remove any trailing newline
        char *newline = strchr(cmdline, '\n');
        if (newline != NULL) {
            *newline = '\0';
        }

        // Evaluate the command line
        eval(cmdline);
    }

    return -1; // control never reaches here
}

/**
 * @brief Runs the child process so the shell can run multiple jobs
 * concurrently, respond to builtin commands, respond to file I/O,
 * and print out error message when executions are not succesful.
 *
 * @param[in] cmdline Char pointer of the command line to be parsed
 *
 * NOTE: The shell is supposed to be a long-running process, so this function
 *       (and its helpers) should avoid exiting on error.  This is not to say
 *       they shouldn't detect and print (or otherwise handle) errors!
 */
void eval(const char *cmdline) {
    parseline_return parse_result;
    struct cmdline_tokens token;
    pid_t pid;
    sigset_t mask_all, prev_all, sigchld, prev, mask_one;
    sigfillset(&mask_all);
    sigemptyset(&sigchld);
    sigaddset(&sigchld, SIGCHLD);
    sigaddset(&sigchld, SIGINT);
    sigaddset(&sigchld, SIGTSTP);
    sigemptyset(&prev);
    sigemptyset(&mask_one);
    sigaddset(&mask_one, SIGCHLD);
    sigaddset(&mask_one, SIGINT);
    sigaddset(&mask_one, SIGTSTP);
    jid_t jid;
    int fdin = 0;
    int fdout = 0;

    // Parse command line
    parse_result = parseline(cmdline, &token); // parse whether bg or fg

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY) {
        return;
    }

    // call helper function builtin
    builtincmd(parse_result, token);

    if (token.builtin != BUILTIN_NONE) {
        return;
    }

    // the call should not be builtin function if reach this stage
    sigprocmask(SIG_BLOCK, &mask_one, &prev_all);
    if (token.infile != NULL) {
        // file input
        fdin = open(token.infile, O_RDONLY);
        if (fdin < 0) {
            if (errno == ENOENT) {
                sio_printf("%s: No such file or directory\n", token.infile);
            } else {
                sio_printf("%s: Permission denied\n", token.infile);
            }
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            return;
        }
    }
    if (token.outfile != NULL) {
        // file output
        fdout = open(token.outfile, O_WRONLY | O_CREAT | O_TRUNC,
                     S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        if (fdout < 0) {
            if (errno == ENOENT) {
                sio_printf("%s: No such file or directory\n", token.outfile);
            } else {
                sio_printf("%s: Permission denied\n", token.outfile);
            }
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            return;
        }
    }

    // fork child to run the program
    if ((pid = fork()) == 0) {
        setpgid(pid, pid);
        if (token.infile != NULL) {
            dup2(fdin, 0);
        }
        if (token.outfile != NULL) {
            dup2(fdout, 1);
        }
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
        if (execve(token.argv[0], token.argv, environ) < 0) {
            fdin = open(token.argv[0], O_RDONLY);
            if (fdin < 0) {
                if (errno == ENOENT) {
                    sio_printf("%s: No such file or directory\n",
                               token.argv[0]);
                } else {
                    sio_printf("%s: Permission denied\n", token.argv[0]);
                }
            }
            exit(0);
        }
        return;
    }

    // add to joblist
    sigprocmask(SIG_BLOCK, &mask_all, NULL);
    if (parse_result == PARSELINE_BG) {
        jid = add_job(pid, BG, cmdline);
    } else if (parse_result == PARSELINE_FG) {
        jid = add_job(pid, FG, cmdline);
    } else {
        sio_printf("not bg or fg\n");
        exit(0);
    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL);

    // decide whether to wait for child process to terminate / stop
    if (parse_result == PARSELINE_FG) {

        // wait for child process to end
        sigprocmask(SIG_BLOCK, &sigchld, NULL);
        int fjid;
        while ((fjid = fg_job()) > 0) {
            job_state state = job_get_state(fjid);
            if (state == ST) {
                sio_printf("job is stopped");
                break;
            }
            sigsuspend(&prev);
        }
        if (token.infile != NULL) {
            close(fdin);
        }
        if (token.outfile != NULL) {
            close(fdout);
        }
        sigprocmask(SIG_SETMASK, &prev, NULL);
    } else if (parse_result == PARSELINE_BG) {

        // print out background job and return
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
        sio_printf("[%d] (%d) %s\n", (int)jid, (int)pid, cmdline);
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }
    return;
}

/**
 * @brief Runs the builtin function to respond to any builtin command call
 *  If the token indicates the command is not a builtin command, the function
 *  does not enter any of the conditionals and will run normally in eval()
 *
 * @param[in] parse_result The parse result from eval() function
 * @param[in] token The token from eval() function
 *
 * This function cases on four builtin command.
 * The builtin commands are:
 *  bg job, fg job, jobs, quit
 */
void builtincmd(parseline_return parse_result, struct cmdline_tokens token) {

    sigset_t mask_all, prev_all, sigchld, prev, mask_one;
    sigfillset(&mask_all);
    sigemptyset(&sigchld);
    sigaddset(&sigchld, SIGCHLD);
    sigaddset(&sigchld, SIGINT);
    sigaddset(&sigchld, SIGTSTP);
    sigemptyset(&prev);
    sigemptyset(&mask_one);
    sigaddset(&mask_one, SIGCHLD);
    sigaddset(&mask_one, SIGINT);
    sigaddset(&mask_one, SIGTSTP);
    int fdout = 0;

    if (token.builtin == BUILTIN_QUIT) {
        exit(0);
    }

    if (token.builtin == BUILTIN_JOBS) {
        // list all background jobs
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
        if (token.outfile != NULL) {
            fdout = open(token.outfile, O_WRONLY | O_CREAT | O_TRUNC,
                         S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if (fdout < 0) {
                if (errno == ENOENT) {
                    sio_printf("%s: No such file or directory\n",
                               token.outfile);
                } else {
                    sio_printf("%s: Permission denied\n", token.outfile);
                }
                sigprocmask(SIG_SETMASK, &prev_all, NULL);
                return;
            }
            list_jobs(fdout);
            close(fdout);
        } else {
            list_jobs(STDOUT_FILENO);
        }
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }

    if (token.builtin == BUILTIN_BG) {
        if (token.argc == 1) {
            sio_printf("bg command requires PID or %%jobid argument\n");
            return;
        }

        char *start = token.argv[1];
        char *num;
        if ((*start) != '%') {
            num = token.argv[1];
        } else {
            num = token.argv[1] + 1;
        }
        pid_t jid = atoi(num);
        if (jid == 0) {
            sio_printf("bg: argument must be a PID or %%jobid\n");
            return;
        }
        const char *cmd;
        sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
        if (job_exists(jid)) {
            // jid is jid
            pid_t pid = job_get_pid(jid);
            cmd = job_get_cmdline(jid);
            sio_printf("[%d] (%d) %s\n", (int)jid, (int)pid, cmd);
            job_set_state(jid, BG);
            killpg(pid, SIGCONT);
        } else {
            // jid is pid
            pid_t pid = jid;
            jid = job_from_pid(pid);
            if (jid == 0) {
                printf("%s: No such job\n", token.argv[1]);
                sigprocmask(SIG_SETMASK, &prev_all, NULL);
                return;
            }
            cmd = job_get_cmdline(jid);
            sio_printf("[%d] (%d) %s\n", (int)jid, (int)pid, cmd);
            job_set_state(jid, BG);
            killpg(pid, SIGCONT);
        }
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }

    if (token.builtin == BUILTIN_FG) {

        if (token.argc == 1) {
            sio_printf("fg command requires PID or %%jobid argument\n");
            return;
        }

        char *start = token.argv[1];
        char *num;
        if ((*start) != '%') {
            num = token.argv[1];
        } else {
            num = token.argv[1] + 1;
        }
        pid_t jid = atoi(num);
        if (jid == 0) {
            sio_printf("fg: argument must be a PID or %%jobid\n");
            return;
        }

        sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
        if (job_exists(jid)) {
            // jid is jid
            job_set_state(jid, FG);
            pid_t pid = job_get_pid(jid);
            killpg(pid, SIGCONT);
            int fjid;
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            sigprocmask(SIG_BLOCK, &sigchld, NULL);
            while ((fjid = fg_job()) > 0) {
                job_state state = job_get_state(fjid);
                if (state == ST) {
                    break;
                    sigprocmask(SIG_SETMASK, &prev_all, NULL);
                }
                sigsuspend(&prev_all);
            }
        } else {
            // jid is pid
            pid_t pid = jid;
            jid = job_from_pid(pid);
            if (jid == 0) {
                printf("%s: No such job\n", token.argv[1]);
                sigprocmask(SIG_SETMASK, &prev_all, NULL);
                return;
            }
            job_set_state(jid, FG);
            killpg(pid, SIGCONT);
            sigprocmask(SIG_SETMASK, &prev_all, NULL);
            int fjid;
            sigprocmask(SIG_BLOCK, &sigchld, NULL);
            while ((fjid = fg_job()) > 0) {
                job_state state = job_get_state(fjid);
                if (state == ST) {
                    break;
                }
                sigsuspend(&prev_all);
            }
        }
        sigprocmask(SIG_SETMASK, &prev_all, NULL);
    }
}

/*****************
 * Signal handlers
 *****************/

/**
 * @brief SIGCHLD signal handler
 *
 * @param[in] sig The singal number
 *
 * This function responds to SIGCHlD signal and reaps all terminated child
 * process and update the job list correspondingly
 */
void sigchld_handler(int sig) {
    sigset_t mask_all, prev_all;
    pid_t pid;
    int olderrno = errno;
    jid_t jid;
    sigemptyset(&mask_all);
    sigaddset(&mask_all, SIGCHLD);
    sigaddset(&mask_all, SIGINT);
    sigaddset(&mask_all, SIGTSTP);
    int status;

    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
    while ((pid = waitpid(-1, &status, (WNOHANG | WUNTRACED))) > 0) {

        jid = job_from_pid(pid);

        // if terminated abnormally, print out message
        if (WIFSIGNALED(status) && jid != 0) {
            int sig = WTERMSIG(status);
            sio_printf("Job [%d] (%d) terminated by signal %d\n", (int)jid,
                       (int)pid, sig);
        }

        // if stopped, print out message and stop the process
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            sio_printf("Job [%d] (%d) stopped by signal %d\n", (int)jid,
                       (int)pid, sig);
            job_set_state(jid, ST);
        }
        if (!WIFSTOPPED(status)) {
            delete_job(jid);
        }
    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL);

    errno = olderrno;
    return;
}

/**
 * @brief SIGINT signal handler
 *
 * @param[in] sig The singal number
 *
 * This function responds to SIGINT signal and send SIGINT to all foreground
 * porcesses in the foreground group
 */
void sigint_handler(int sig) {

    sigset_t mask_all, prev_all;
    int olderrno = errno;
    jid_t jid;
    sigemptyset(&mask_all);
    sigaddset(&mask_all, SIGCHLD);
    sigaddset(&mask_all, SIGINT);
    sigaddset(&mask_all, SIGTSTP);

    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
    jid = fg_job();

    if (jid) {
        pid_t pid = job_get_pid(jid);
        killpg(pid, SIGINT);
    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL);

    errno = olderrno;
    return;
}

/**
 * @brief SIGTSTP signal handler
 *
 * @param[in] sig The singal number
 *
 * This function responds to SIGTSTP signal and send SIGTSTP to all foreground
 * porcesses in the foreground group
 */
void sigtstp_handler(int sig) {
    sigset_t mask_all, prev_all;
    int olderrno = errno;
    jid_t jid;
    sigemptyset(&mask_all);
    sigaddset(&mask_all, SIGCHLD);
    sigaddset(&mask_all, SIGINT);
    sigaddset(&mask_all, SIGTSTP);

    sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
    jid = fg_job();

    if (jid) {
        pid_t pid = job_get_pid(jid);
        killpg(pid, SIGTSTP);
    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL);

    errno = olderrno;
    return;
}

/**
 * @brief Attempt to clean up global resources when the program exits.
 *
 * In particular, the job list must be freed at this time, since it may
 * contain leftover buffers from existing or even deleted jobs.
 */
void cleanup(void) {
    // Signals handlers need to be removed before destroying the joblist
    Signal(SIGINT, SIG_DFL);  // Handles Ctrl-C
    Signal(SIGTSTP, SIG_DFL); // Handles Ctrl-Z
    Signal(SIGCHLD, SIG_DFL); // Handles terminated or stopped child

    destroy_job_list();
}
