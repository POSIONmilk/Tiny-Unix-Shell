/* 
 * tsh - A tiny shell program with job control
 * 
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */
#define TRUE 1
#define FALSE 0

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* Per-job data */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, FG, BG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

volatile sig_atomic_t ready; /* Is the newest child in its own process group? */

/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);
void sigusr1_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int freejid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(STDOUT_FILENO, STDERR_FILENO);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != -1) {
        switch (c) {
            case 'h':             /* print help message */
                usage();
                break;
            case 'v':             /* emit additional diagnostic info */
                verbose = 1;
                break;
            case 'p':             /* don't print a prompt */
                emit_prompt = 0;  /* handy for automatic testing */
                break;
            default:
                usage();
        }
    }

    /* Install the signal handlers */

    Signal(SIGUSR1, sigusr1_handler); /* Child is ready */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* pipe_tokenizer - takes a string as input, and splits it into an array of strings 
 * based on any '|'.
 *
 * Returns the number of elements in the array
*/
int pipe_tokenizer(const char *cmdline, char **output){
    char array[MAXLINE]; // A local copy of the command line
    strcpy(array, cmdline); // Copy the cmdline to array
    int i = 0;
    char *delimiter = "|";
    char *token;
    token = strtok(array, delimiter);
    while(token != NULL){
        output[i] = strdup(token); // Copy the token to output
        if (!output[i]) {
            perror("strdup");
            exit(EXIT_FAILURE);
        }
        i += 1;
        token = strtok(NULL, delimiter); // Move to the next token
    }
    return i;
}

/* pipe_eval - evaluates piped commands
 *
 *
 *
*/
void pipe_eval(char **pipedarg, int pipenumber){

    int fd[2];
    int standard_in = dup(STDIN_FILENO);
    int standard_out = dup(STDOUT_FILENO);

    for(int arg = 0; arg < pipenumber; arg++){
        char *parsed_arg[MAXARGS];
        int parsed_argc;
        char * argv_no_redirc[MAXARGS];

        int counter = 0;

        parsed_argc = parseline(pipedarg[arg], parsed_arg);
        if(parsed_argc == 0){
            printf("Incorrect Usage of pipe\n");
            return;
        }

        for(int i = 0; i < parsed_argc; i++){
            if(strcmp(parsed_arg[i], "<") == 0){
                i ++;
            }else if(strcmp(parsed_arg[i], ">") == 0){
                i ++;
            }else{
                argv_no_redirc[counter] = parsed_arg[i];
                counter ++;
            }
        }
        argv_no_redirc[counter] = NULL;

        // Setting up pipes
        // First file that is being piped
        
        if(arg == 0){
            if(pipe(fd) == -1){perror("pipe"); exit(-1);}
            if(dup2(fd[1], STDOUT_FILENO) == -1) {
                perror("dup2");
                exit(-1);
            }
            close(fd[1]);
        }
        else if (arg == pipenumber - 1){
            if(dup2(fd[0], STDIN_FILENO) == -1){perror("dup2"); exit(-1);}
            if(dup2(standard_out,STDOUT_FILENO) == -1){perror("dup2"); exit(-1);}
        }
        else{
            if(dup2(fd[0], STDIN_FILENO) == -1){perror("dup2"); exit(-1);}
            if(pipe(fd) == -1){perror("pipe"); exit(-1);}
            if(dup2(fd[1], STDOUT_FILENO) == -1) {perror("dup2"); exit(-1);}
            close(fd[1]);
        }
        
        // Forking to create the children
        int pid = fork();
        if(pid == -1){perror("fork");exit(-1);}

        if(pid == 0){ // In the child, exec
            execv(argv_no_redirc[0], argv_no_redirc);
            printf("%s: Command not found\n", argv_no_redirc[0]);
            exit(1);
        }
        else{ // In the parent
            dup2(standard_in, STDIN_FILENO);
            dup2(standard_out, STDOUT_FILENO);
            // waitpid(pid, NULL, 0);
        }   
    }
    for (int k = 0; k < pipenumber; k++) {
        waitpid(-1, NULL, WUNTRACED);
    }
    exit(0);
}



/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) {
    // Parsing the command line
	char *argv[MAXARGS];
    int argc;
    argc = parseline(cmdline, argv);
    // Check if its a builtin command, if so, send it to builtin_cmd
    if (argc == 0){
        return;
    }
    else if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "jobs") == 0 || strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "bg") == 0){
	    builtin_cmd(argv);
    }
    else{
        // Blocking signals with setmask
        sigset_t set, oldset;
        sigemptyset(&set);
        sigfillset(&set);
        sigprocmask(SIG_BLOCK, &set, &oldset);
        // Checking for input output redirection (YET TO BE IMPLEMENTED)
        // Checking if it runs in background or foreground
        int fg = TRUE;
        if(strcmp(argv[argc-1], "&") == 0){fg = FALSE;}
            

        // Add to job list, deal with making sure that the child doesn't 
        // send stop signal before the job is added. May have to write code 
        // before the fork. Check hint 1
        
        int input_red = 0;
        char* input_red_file;
        int output_red = 0;
        char* output_red_file;

        char * argv_no_redirc[MAXARGS];
        int counter = 0;

        // Fork Process
        int pid = fork();
        
        for(int i = 0; i < argc; i++){
            if(strcmp(argv[i], "<") == 0){
                input_red = 1;
                input_red_file = argv[i+1];
                // printf("Input redirect required into: %s\n",input_red_file);
                i ++;
            }else if(strcmp(argv[i], ">") == 0){
                output_red = 1;
                output_red_file = argv[i+1];
                // printf("Output redirect required into: %s\n",output_red_file);
                i++;
            }else{
                argv_no_redirc[counter] = argv[i];
                counter ++;
            }
        }
        argv_no_redirc[counter] = NULL;
        
        char *pipedarg[MAXARGS];
        int pipenumber = pipe_tokenizer(cmdline, pipedarg);
        
        // Run in foreground
        if (fg) {
            // Child setting its own process group
            if (pid == 0) {
                Signal(SIGINT, SIG_IGN);
                Signal(SIGTSTP, SIG_IGN);
                Signal(SIGCHLD, SIG_IGN);

                setpgid(0,0);
                if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1){
                    perror("sigprocmask() error");
                }
                if(input_red){
                    int input_file = open(input_red_file, O_RDONLY);
                    if(input_file == -1){
                        perror("open");
                    }
                    if(dup2(input_file ,fileno(stdin)) == -1){
                        perror("Error redirecting stdin");
                        exit(EXIT_FAILURE);
                    }
                }   
                if(output_red){
                    int output_file = open(output_red_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if(output_file == -1){
                        perror("File open error");
                    }
                    if (dup2( output_file, fileno(stdout)) == -1){
                        perror("Error redirecting stdout");
                        exit(EXIT_FAILURE);
                    }
                }
                // Checking if we need to pipe
                if(pipenumber > 1){
                    Signal(SIGINT, SIG_DFL);
                    Signal(SIGTSTP, SIG_DFL);
                    Signal(SIGCHLD, SIG_DFL);
                    pipe_eval(pipedarg, pipenumber);
                }
                // We don't need to pipe
                else{
                    Signal(SIGINT, SIG_DFL);
                    Signal(SIGTSTP, SIG_DFL);
                    Signal(SIGCHLD, SIG_DFL);
                    execv(argv[0], argv_no_redirc);
                    printf("%s: Command not found\n", argv[0]);
                    exit(1);
                }
            } 
            // Parent setting child's process group and adding to the job
            else {
                setpgid(pid, pid);
                addjob(jobs, pid, FG, cmdline);
                if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1){
                    perror("sigprocmask() error");
                }
                waitfg(pid);
            }
            
        }
        // Run in background
        else{
            if (pid == 0) {
                Signal(SIGINT, SIG_IGN);
                Signal(SIGTSTP, SIG_IGN);
                Signal(SIGCHLD, SIG_IGN);   
                argv[argc - 1] = '\0';
                
                if(input_red){
                    argv_no_redirc[counter-1] = '\0';
                }
                else if(output_red){
                    argv_no_redirc[counter-1] = '\0';

                }else if(input_red && output_red){
                    argv_no_redirc[counter-1] = '\0';
                }else{
                    argv_no_redirc[counter - 1] = '\0';

                }

                setpgid(0,0);
                if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1){
                    perror("sigprocmask() error");
                }
                if(input_red){
                    int input_file = open(input_red_file, O_RDONLY);
                    if(input_file == -1){
                        perror("open");
                    }
                    dup2(input_file ,fileno(stdin));
                }   
                if(output_red){
                    int output_file = open(output_red_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if(output_file == -1){
                        perror("open");
                    }
                    if (dup2( output_file, fileno(stdout)) == -1){
                        perror("Error redirecting stdout");
                        exit(EXIT_FAILURE);
                    }
                    // printf("Output redirect\n");
                }
                // Checking if we need to pipe
                if(pipenumber > 1){
                    Signal(SIGINT, SIG_DFL);
                    Signal(SIGTSTP, SIG_DFL);
                    Signal(SIGCHLD, SIG_DFL);
                    int length_of_last = strlen(pipedarg[pipenumber-1]);
                    pipedarg[pipenumber-1][length_of_last-1] = '\0';
                    pipe_eval(pipedarg, pipenumber);
                }
                // We don't need to pipe
                else{
                    Signal(SIGINT, sigint_handler);
                    Signal(SIGTSTP, sigtstp_handler);
                    Signal(SIGCHLD, sigchld_handler);
                    execv(argv[0], argv_no_redirc);
                    printf("%s: Command not found\n", argv[0]);
                    exit(1);
                }
            }else{
                setpgid(pid, pid);
                addjob(jobs, pid, BG, cmdline);
                int jid = getjobpid(jobs, pid)->jid;

                //Printing the jid, pid, and argv
                printf("[%d] (%d)", jid, pid);
                for(int i = 0; i<argc; i++){
                    printf(" %s", argv[i]);
                }
                printf("\n");
                
                if (sigprocmask(SIG_SETMASK, &oldset, NULL) == -1){
                    perror("sigprocmask() error");
                }
            }

        }
            
    }

}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return number of arguments parsed.
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to space or quote delimiters */
    int argc;                   /* number of args */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    return argc;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) {
    // Check if its quit, fg, bg, or jobs

    char *cmd = argv[0];
    if(strcmp( cmd, "quit" ) == 0){
      exit(1);
    }
    else if (strcmp( cmd, "fg" ) == 0){
      do_bgfg(argv);
    }
    else if (strcmp( cmd, "bg" ) == 0){
	  //run bg - SHIREN | TRACE 9
      do_bgfg(argv);
    }
    else if (strcmp( cmd, "jobs" ) == 0){
	  //run jobs - SHIREN | TRACE 5
      listjobs(jobs);
      return 0;
    }
return 0;

}

/* 
 * do_bgfg - Execute the builtin bg and fg commands -SHIREN | TRACE 9
 */
void do_bgfg(char **argv) {
    struct job_t *jobfound;
    if (!argv[1]) { // Ensure there is an argument
        printf("%s command requires PID or %%jid argument\n", argv[0]);
        return;
    }
    
    // Does not have % therefore is JID
    if(argv[1][0] == '%'){
        int jid = strtol(&argv[1][1], NULL, 10);
        // Check for conversion errors
        if(jid == 0){
            printf("%s: argument must be a PID or a %%jid\n", argv[0]);
            return;
        }
        jobfound = getjobjid(jobs, jid);
    }
    // Has null therfore is PID
    else{
        pid_t pid = strtol(argv[1], NULL, 10);

        if(pid == 0){
            printf("%s: argument must be a PID or a %%jid\n", argv[0]);
            return;
        }  
        jobfound = getjobpid(jobs, pid);
    }
    
    if(jobfound != NULL){
        int jidSOLO = jobfound->jid;
        pid_t pidSOLO = jobfound->pid;
        if(strcmp(argv[0], "bg") == 0){
            jobfound->state = BG;
            
            printf("[%d] (%d) %s", jidSOLO, pidSOLO, jobfound->cmdline);
            
            kill(-pidSOLO, SIGCONT);
        } 
        else {
            jobfound->state = FG;
            kill(-pidSOLO, SIGCONT);
            waitfg(jobfound->pid);
        }
        
    }
    else{
      if(argv[1][0]== '%'){
        printf("%s: No such job\n", argv[1]);
        return;
      }
      else{
        printf("(%s): No such process\n", argv[1]);
        return;
      }
    }
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
   // Get the process group of the id passed as well as the foreground process group
    pid_t pidPG = getpgid(pid);
    struct job_t *job = getjobpid(jobs, pid);
    pid_t fgPG = fgpid(jobs);

    // setup an empty mask for sigsuspend
    sigset_t mask;
    sigemptyset(&mask);
    int inForeground = TRUE;

    // If the process is currently in the foreground, suspend until you get a signal.
    // Once we have a signal, check if the process is still in the foreground.
    // If it is, loop back and continue blocking, if it isn't kill the loop and return.
    if(pidPG == fgPG){
        while(inForeground){
            if (sigsuspend(&mask) == -1 && errno != EINTR){
                perror("sigsuspend");
            }
            pidPG = getpgid(pid);
            if(pidPG != fgPG || job->state != 1){
                inForeground = FALSE;
            }
        }
    }
    return;

}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) {
    pid_t reapedPID;
    int status;

    while ((reapedPID = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0){
        struct job_t *job = getjobpid(jobs, reapedPID);
        if(WIFSTOPPED(status)){
            job->state = ST;
            printf("Job [%d] (%d) stopped by signal %d\n", job->jid, job->pid, SIGTSTP);
            return;
        }
        if(WIFCONTINUED(status)){
            return;
        }
        else{
            // Child was either exited normally or was terminated
            if(!WIFEXITED(status)){
              printf("Job [%d] (%d) terminated by signal %d\n", job->jid, job->pid, WTERMSIG(status));
            }
            deletejob(jobs, reapedPID);
            return;
        }
    }
return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job. - KAIRUI | TRACE 6,7
 */
void sigint_handler(int sig) {
    pid_t fgp = fgpid(jobs);
    if(fgp != 0){
        // struct job_t *job = getjobpid(jobs, fgp);
        if(kill(-fgp, sig)==-1){
            perror("sigint error");
        }
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP. - GAO | TRACE 8
 */
void sigtstp_handler(int sig) {
    pid_t fgp = fgpid(jobs);
    if(fgp == 0){
        return;
    }
    // struct job_t *job = getjobpid(jobs, fgp);
    if(kill(-fgp, sig) == -1){
        perror("sigtstp");
    }
    return;
}

/*
 * sigusr1_handler - child is ready
 */
void sigusr1_handler(int sig) {
    ready = 1;
}


/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* freejid - Returns smallest free job ID */
int freejid(struct job_t *jobs) {
    int i;
    int taken[MAXJOBS + 1] = {0};
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid != 0) 
        taken[jobs[i].jid] = 1;
    for (i = 1; i <= MAXJOBS; i++)
        if (!taken[i])
            return i;
    return 0;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    int i;
    
    if (pid < 1)
        return 0;
    int free = freejid(jobs);
    if (!free) {
        printf("Tried to create too many jobs\n");
        return 0;
    }
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = free;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    return 0; /*suppress compiler warning*/
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) {
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
                case BG: 
                    printf("Running ");
                    break;
                case FG: 
                    printf("Foreground ");
                    break;
                case ST: 
                    printf("Stopped ");
                    break;
                default:
                    printf("listjobs: Internal error: job[%d].state=%d ", 
                       i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message and terminate
 */
void usage(void) {
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg) {
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg) {
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) {
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
