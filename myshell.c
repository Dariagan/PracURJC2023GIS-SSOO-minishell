#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <pthread.h>
#define true 1
#define false 0
#define BUFFER_SIZE 2048
#define READING_END 0
#define WRITING_END 1
int execute_exit();
typedef enum{RUNNING, SUSPENDED, DONE, FAILED} JobState;
const char* const job_state_to_string(const JobState job_state)
{switch(job_state){
case RUNNING: return "Running";case SUSPENDED: return "Suspended"; case DONE: return "Done"; case FAILED: return "Failed"; 
default:fprintf(stderr, "INTERNAL ERROR at job_state_to_string()\n"); execute_exit();
}}
typedef struct 
{
    unsigned int unique_id;
    tline line;
    JobState state;
    int exec_exit_status;
    pid_t* children_arr;
    unsigned int currently_awaited_child_cmd_i;
    pthread_t handler_thread_id;
} Job;

static pthread_mutex_t reading_or_modifying_bg_jobs_mtx;

//NO ITERAR SOBRE O MODIFICAR SIN MUTEX ⚠️
static Job* bg_jobs;
//NO MODIFICAR SIN MUTEX ⚠️
static unsigned int bg_jobs_arr_size = 0;
void deep_free_line_embedded_strings(Job* job);

// SOLO LLAMAR DESDE DENTRO DEL MUTEX ⚠️
void remove_job_from_bgjobs_arr(const unsigned int job_to_remove_uid)
{
    Job* prev_bg_jobs;
    unsigned int prev_arr_i, added_jobs_count = 0; 
    if(bg_jobs_arr_size == 0) {fprintf(stderr, "INTERNAL ERROR: job list is empty at this execution point\n"); kill(0, 9);}

    prev_bg_jobs = bg_jobs;
    if(-- bg_jobs_arr_size > 0)
    {
        bg_jobs = malloc((bg_jobs_arr_size)*sizeof(Job));
        for(prev_arr_i = 0; prev_arr_i < bg_jobs_arr_size + 1; prev_arr_i++)
        {   
            if(prev_bg_jobs[prev_arr_i].unique_id != job_to_remove_uid)
            {
                bg_jobs[added_jobs_count++] = prev_bg_jobs[prev_arr_i];
            }
            else deep_free_line_embedded_strings(prev_bg_jobs + prev_arr_i);
        }
    }free(prev_bg_jobs);
}

void update_job_state(const unsigned int job_uid, const JobState new_state, const int code)
{
    int i; 
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    for(i = 0; i < bg_jobs_arr_size; i++) 
        if(bg_jobs[i].unique_id == job_uid)
        {
            bg_jobs[i].state = new_state;
            bg_jobs[i].exec_exit_status = code;
            break;
        }
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
}
//cambiar el i del comando en el que estamos al procesar el trabajo desde el background
void increment_awaited_child_cmd_i(
    const unsigned int job_uid)
{
    int i;
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    for(i = 0; i < bg_jobs_arr_size; i++)
        if(bg_jobs[i].unique_id == job_uid)
        {
            bg_jobs[i].currently_awaited_child_cmd_i ++;
            break;
        }
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
}
//Esto es para hacer una copia profunda de los strings embebidos en la tline src_line, a dest_line.
//Si no se hace esto, la siguiente llamada a tokenize() sobrescribiría los strings apuntados, la copia de solo punteros no sirve.
void deep_copy_line(tline* dest_line, tline* src_line)
{
    int cmd_i, arg_j; tcommand* dest_command; tcommand* src_command;
    *dest_line = *src_line;
    dest_line->commands = malloc(src_line->ncommands*sizeof(tcommand));

    for(cmd_i = 0; cmd_i < src_line->ncommands; cmd_i++)
    {
        dest_command = dest_line->commands + cmd_i;
        src_command = src_line->commands + cmd_i;

        dest_command->filename = strdup(src_command->filename);
        dest_command->argc = src_command->argc;
        dest_command->argv = malloc(src_command->argc*sizeof(char*));
        for(arg_j = 0; arg_j < src_command->argc; arg_j++)
            dest_command->argv[arg_j] = strdup(src_command->argv[arg_j]);
    }
    if(src_line->redirect_input)
        dest_line->redirect_input = strdup(src_line->redirect_input);
    if(src_line->redirect_output)
        dest_line->redirect_output = strdup(src_line->redirect_output);
    if(src_line->redirect_error)
        dest_line->redirect_error = strdup(src_line->redirect_error);
}
void deep_free_line_embedded_strings(Job* job)
{
    int cmd_i, arg_j; 
    tline* line = &(job->line);
    tcommand* current_command;
    if(line->redirect_input)
        free(line->redirect_input);
    if(line->redirect_output)
        free(line->redirect_output);
    if(line->redirect_error)
        free(line->redirect_error);
    for(cmd_i = 0; cmd_i < line->ncommands; cmd_i++)
    {
        current_command = line->commands + cmd_i;
        free(current_command->filename);
        for(arg_j = 0; arg_j < current_command->argc; arg_j++)
        {
            free(current_command->argv[arg_j]);
        }
        free(current_command->argv);
    }
    free(line->commands);
}
//NO MODIFICAR SIN MUTEX ⚠️
static unsigned int next_job_uid_to_assign = 1;

static pthread_t foreground_thread;
static pid_t* fg_forks_pids_arr;
static unsigned int fg_n_commands = 0;

static unsigned int fg_awaited_child_cmd_i = 0;

static bool fg_execution_cancelled = false;

typedef struct {int** used_pipes_arr; pid_t* children_pids_arr; tline line;} AddJobArgs;
void* async_add_bg_job_and_cleanup_after_it(void* uncasted_args)
{
    AddJobArgs args = *((AddJobArgs*)uncasted_args);free(uncasted_args);
    unsigned int cmd_i; int ch_status; 
    Job this_job;
    this_job.state = RUNNING;
    this_job.children_arr = args.children_pids_arr;

    this_job.line = args.line;
    this_job.exec_exit_status = 0;

    this_job.handler_thread_id = pthread_self();
    this_job.currently_awaited_child_cmd_i = 0;
    
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);

    this_job.unique_id = next_job_uid_to_assign ++;

    if(bg_jobs_arr_size ++ == 0) bg_jobs = malloc(sizeof(Job));
    else
        bg_jobs = realloc(bg_jobs, bg_jobs_arr_size*sizeof(Job));

    bg_jobs[bg_jobs_arr_size - 1] = this_job;
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);

    for(cmd_i = 0; cmd_i < this_job.line.ncommands; cmd_i++)
    {
        if( ! (pthread_self()==foreground_thread && fg_execution_cancelled) 
            && this_job.state!=FAILED)
        {
            waitpid(this_job.children_arr[cmd_i], &ch_status, 0);
            increment_awaited_child_cmd_i(this_job.unique_id);

            if(WIFEXITED(ch_status) && WEXITSTATUS(ch_status))
            {
                this_job.exec_exit_status = WEXITSTATUS(ch_status);
                this_job.state = FAILED;
                if(pthread_self() != foreground_thread)
                    update_job_state(this_job.unique_id, this_job.state, this_job.exec_exit_status);
            }
        }else{
            kill(this_job.children_arr[cmd_i], SIGTERM);
            usleep(50);
            if(!waitpid(this_job.children_arr[cmd_i], NULL, WNOHANG))
            {
                if(pthread_self() != foreground_thread) sleep(1); 

                kill(this_job.children_arr[cmd_i], SIGKILL);
            }
        }
        if (cmd_i < this_job.line.ncommands - 1) 
            free((args.used_pipes_arr)[cmd_i]);
    }
    if(pthread_self() != foreground_thread && this_job.state != FAILED)
    {    
        update_job_state(this_job.unique_id, DONE, 0);
    }
    if( ! (pthread_self()==foreground_thread&&fg_execution_cancelled))
        free(args.children_pids_arr);

    free(args.used_pipes_arr); 
    pthread_exit((void*)(long)this_job.exec_exit_status);
}
//USAR DENTRO DE MUTEX ⚠️. Devuelve un puntero que apunta al trabajo que tenga el identificador único pasado, o NULL si no lo encuentra.
Job* find_bg_job(unsigned int uid){
    unsigned int i;
    for(i = 0; i < bg_jobs_arr_size; i++)
        if (bg_jobs[i].unique_id == uid)
            return bg_jobs + i; 
    return NULL;
}
//------------------------------------------------------------------
//-------------------------COMANDOS NATIVOS-------------------------
static const char CD[] = "cd"; static const char JOBS[] = "jobs";
static const char FG[] = "fg"; static const char EXIT[] = "exit";
static const char UMASK[] = "umask";
static const char * const BUILTIN_COMMANDS[] = {CD, JOBS, FG, EXIT, UMASK};
static const __u_char N_BUILTIN_COMMANDS = sizeof(BUILTIN_COMMANDS)/sizeof(char*);

int execute_cd(tcommand* command_data)
{
    const char* target_directory = command_data->argv[1];

    if (command_data->argc != 2) {
        fprintf(stderr, "Usage: %s <directory>\n", CD);
        return EXIT_FAILURE;
    }
    if (strcmp(target_directory, "~") == 0)
    {
        target_directory = getenv("HOME");
        if (target_directory == NULL) {
            fprintf(stderr, "Failure: HOME environment variable not set.\n");
            return EXIT_FAILURE;
        }
    }
    if (chdir(target_directory) != 0) {
        perror(CD);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
int execute_jobs(tcommand* command_data)
{
    int job_i, cmd_i, arg_j, jobs_to_remove_count = 0; 
    unsigned int* completed_job_uids;
    Job* job;
    tcommand* command;
    if (command_data->argc != 1) {
        fprintf(stderr, "Usage: %s \n", JOBS);
        return EXIT_FAILURE;
    }
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    for(job_i = 0; job_i < bg_jobs_arr_size; job_i++)
    {
        job = bg_jobs + job_i;
        printf("[%dº][UID:%u] job: ", job_i+1, job->unique_id);
        for(cmd_i = 0; cmd_i < job->line.ncommands; cmd_i++)
        {
            command = job->line.commands + cmd_i;
            for(arg_j = 0; arg_j < command->argc; arg_j++)
                printf("%s ", command->argv[arg_j]);
            if(cmd_i <  job->line.ncommands - 1)
                printf("| ");
        }
        if(job->line.redirect_input)
            printf(" < %s ", job->line.redirect_input);
        if(job->line.redirect_output)
            printf(" > %s ", job->line.redirect_output);
        if(job->line.redirect_error)
            printf(" >& %s ", job->line.redirect_error);

        printf("STATUS: %s", job_state_to_string(job->state));

        if(job->state == DONE || job->state == FAILED)
        {
            printf("(%d)", job->exec_exit_status);

            if(jobs_to_remove_count ++ == 0)
            {
                completed_job_uids = malloc(sizeof(*completed_job_uids));
                completed_job_uids[0] = job->unique_id;
            }
            else{
                completed_job_uids = realloc(completed_job_uids, jobs_to_remove_count*sizeof(*completed_job_uids));
                completed_job_uids[jobs_to_remove_count-1] = job->unique_id;
            }
        }
        putchar('\n');
    }
    for(job_i = 0; job_i < jobs_to_remove_count; job_i++)
    {
        remove_job_from_bgjobs_arr(completed_job_uids[job_i]);
    }
    pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
    
    if(jobs_to_remove_count > 0)
        free(completed_job_uids);
    
    return EXIT_SUCCESS;
}
int execute_fg(tcommand* command_data)
{
    long uid; Job* job; int thread_return;
    if (command_data->argc != 2) {
        fprintf(stderr, "Usage: %s <job UID>\n", FG);
    }
    else if(uid = atol(command_data->argv[1]))
    {   
        pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
        
        if(job = find_bg_job(uid))
            if(job->state != DONE && job->state != FAILED)
            {
                fg_awaited_child_cmd_i = job->currently_awaited_child_cmd_i;
                fg_forks_pids_arr = job->children_arr;
                fg_n_commands = job->line.ncommands;
                foreground_thread = job->handler_thread_id;
                remove_job_from_bgjobs_arr(uid);
                pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
                pthread_join(foreground_thread, (void**)&thread_return);

                foreground_thread = pthread_self();
                return thread_return;
            }
            else fprintf(stderr, "Job with UID=%ld has already finished its execution\n", uid);
        else fprintf(stderr, "Job with UID=%ld not found\n", uid);
        
        pthread_mutex_unlock(&reading_or_modifying_bg_jobs_mtx);
    }
    else fprintf(stderr, "Specified job-UID must be a strictly positive integer\n");

    return EXIT_FAILURE;
}
int execute_exit(){
    const char ALL = 0;
    pthread_mutex_lock(&reading_or_modifying_bg_jobs_mtx);
    signal(SIGTERM, SIG_IGN);
    kill(ALL, SIGTERM);
    sleep(1);
    kill(ALL, SIGKILL);
    exit(0);
}
int execute_umask(tcommand* command_data)
{
    mode_t new_mask;
    if (command_data->argc != 2) {
        fprintf(stderr, "Usage: %s <octal-mask>\n", UMASK);
        return EXIT_FAILURE;
    }
    if (sscanf(command_data->argv[1], "%o", &new_mask) != 1) {
        fprintf(stderr, "Failure: Invalid octal mask format.\n");
        return EXIT_FAILURE;
    }
    umask(new_mask); return EXIT_SUCCESS;
}
//Ejecutar un comando interno que esté implementado
int execute_built_in_command(tcommand* command_data)
{
    const char* const command_name = command_data->argv[0];
    if(strcmp(CD, command_name) == 0)
    {
        return execute_cd(command_data);
    }
    else if(strcmp(JOBS, command_name) == 0)
    {
        return execute_jobs(command_data);
    }
    else if(strcmp(FG, command_name) == 0)
    {
        return execute_fg(command_data);
    }
    else if(strcmp(EXIT, command_name) == 0)
    {
        return execute_exit();
    }
    else if(strcmp(UMASK, command_name) == 0)
    {
        return execute_umask(command_data);
    }
    fprintf(stderr, "INTERNAL ERROR: BUILT-IN COMMAND \"%s\" IS NOT HANDLED BY PROGRAM\n", command_name);
    execute_exit();
}
bool is_builtin_command(tcommand* command_data)
{
    const char* const command_name = command_data->argv[0];
    int i;
    for(i = 0; i < N_BUILTIN_COMMANDS; i++)
        if(strcmp(BUILTIN_COMMANDS[i], command_name) == 0)
            return true;
    return false;
}
//------------------------/COMANDOS NATIVOS-------------------------
//------------------------------------------------------------------

typedef struct{pid_t* forks_pids_arr; int awaited_i; int n_commands;}AsyncKillArgs;
void* async_delayed_force_kill(void * uncasted_args)
{
    int i; AsyncKillArgs args = *((AsyncKillArgs*)uncasted_args);
    free(uncasted_args); sleep(5);
    for(i = args.awaited_i; i < args.n_commands; i++)
        kill(args.forks_pids_arr[i], SIGKILL);
    free(args.forks_pids_arr); return NULL;
}
//manejador de la señal SIGINT (ctrl + c) para el proceso padre. (los procesos hijos la ignoran)
static bool sent_to_background = false;
void stop_foreground_execution(int signal)
{
    unsigned int i; AsyncKillArgs* args; pthread_t placeholder;

    if(signal == SIGINT && !sent_to_background && fg_n_commands)
    {
        for(i = fg_awaited_child_cmd_i; i < fg_n_commands; i++)
        {// se señala que terminen su ejecución ordenadamente
            kill(fg_forks_pids_arr[i], SIGTERM);
        }
        fg_execution_cancelled = true;
        args = malloc(sizeof(AsyncKillArgs));
        args->n_commands = fg_n_commands;
        args->forks_pids_arr = fg_forks_pids_arr;
        args->awaited_i = fg_awaited_child_cmd_i;
        
        if(pthread_create(&placeholder, NULL, async_delayed_force_kill, (void*)args) != 0)
        {//si falla la creación del thread:
            for(i = args->awaited_i; i < args->n_commands; i++)
                kill(args->forks_pids_arr[i], SIGKILL);
            free(args->forks_pids_arr);
        }
    }
}
void fully_close_pipe(const int pipe[2]) {close(pipe[0]); close(pipe[1]);}

void close_non_adjacent_pipes(int ** pipes, const int cmd_i, const int N_PIPES)
{   int j;//se cierran las de atrás:
    for(j = cmd_i - 2; j >= 0; j--) fully_close_pipe(pipes[j]);
    //se cierran las de adelante:
    for(j = cmd_i + 1; j < N_PIPES; j++) fully_close_pipe(pipes[j]);
}
int run_line(tline* line)
{
    const unsigned int N_PIPES = line->ncommands - 1;
    int** pipes_arr;
    bool builtin_command_present = false;
    bool input_from_file = line->redirect_input != NULL;
    bool output_to_file = line->redirect_output != NULL;
    bool output_stdout_and_err_to_file = line->redirect_error != NULL;
    int exec_exit_status = 0;
    //variables temp:
    pid_t current_pid; FILE *file; int i; pthread_t placeholder; int ch_status;
    AddJobArgs *args;
    
    fg_n_commands = N_PIPES + 1;
    sent_to_background = line->background;
    fg_forks_pids_arr = (pid_t*)malloc(fg_n_commands*sizeof(pid_t));

    if(!fg_n_commands) return 0;

    for(i = 0; i < fg_n_commands && !builtin_command_present; i++)
    {
        builtin_command_present = is_builtin_command(line->commands + i);
    }
    if(builtin_command_present) 
    {
        if(fg_n_commands > 1){
            fprintf(stderr, "Error: built-in commands cannot be piped\n");
            return EXIT_FAILURE;
        }
        if(input_from_file){
            fprintf(stderr, "Error: built-in commands cannot take input from a file\n");
            return EXIT_FAILURE;
        }
        if(output_to_file){
            fprintf(stderr, "Error: built-in commands cannot output to a file\n");
            return EXIT_FAILURE;
        }
        if(line->background){
            fprintf(stderr, "Error: built-in commands cannot be executed in the background\n");
            return EXIT_FAILURE;
        }
        return execute_built_in_command(line->commands + 0);
    }
    pipes_arr = (int **)malloc((N_PIPES)*sizeof(int*));

    for(i = 0; i < N_PIPES; i++)
    {
        pipes_arr[i] = (int*)malloc(2*sizeof(int));
        pipe(pipes_arr[i]);
    }
    
    for(i = 0; i < fg_n_commands; i++)
    {
        current_pid = fork();

        if(current_pid == 0)//hijo
        {
            //ignorar los ctrl + c
            signal(SIGINT, SIG_IGN);

            if(N_PIPES >= 1)
            {
                close_non_adjacent_pipes(pipes_arr, i, N_PIPES);
                if(i == 0)//primer comando
                {
                    close(pipes_arr[i][READING_END]);
                    dup2 (pipes_arr[i][WRITING_END], STDOUT_FILENO);
                    close(pipes_arr[i][WRITING_END]);
                }
                else if(i < fg_n_commands - 1)//comando intermedio
                {
                    close(pipes_arr[i - 1][WRITING_END]);
                    dup2 (pipes_arr[i - 1][READING_END], STDIN_FILENO);
                    close(pipes_arr[i - 1][READING_END]);

                    close(pipes_arr[i][READING_END]);
                    dup2 (pipes_arr[i][WRITING_END], STDOUT_FILENO);
                    close(pipes_arr[i][WRITING_END]);
                }
                else//último comando
                {
                    close(pipes_arr[i - 1][WRITING_END]);
                    dup2 (pipes_arr[i - 1][READING_END], STDIN_FILENO);
                    close(pipes_arr[i - 1][READING_END]);
                }
            }
            if (access(line->commands[i].filename, F_OK) != 0) {
               fprintf(stderr, "Failure: Command \"%s\" not found\n", line->commands[i].argv[0]);
               exit(EXIT_FAILURE);
            }
            if(i == 0 && input_from_file)
            {//si es el primer comando y hay q usar un fichero como stdin
                file = freopen(line->redirect_input, "r", stdin);
                if (file == NULL) {
                    fprintf(stderr, "Failed to open file \"%s\" as input\n", line->redirect_input);
                    exit(EXIT_FAILURE);
                }
            }
            if(i == fg_n_commands - 1)//si es el último comando...
            {
                if(output_to_file)
                {
                    file = freopen(line->redirect_output, "w", stdout);
                    if (file == NULL) {
                        fprintf(stderr, "Failed to create file for outputting stdout\n");
                        exit(EXIT_FAILURE);
                    }
                }
                if(output_stdout_and_err_to_file)
                {
                    file = freopen(line->redirect_error, "w", stderr);
                    if (file == NULL) {
                        fprintf(stderr, "Failed to create file for outputting stderr\n");
                        exit(EXIT_FAILURE);
                    }
                }
            }
            if(i > 0)//esperar q el hermano anterior esté muerto (cuando la señal devuelve 1)
                while(kill(fg_forks_pids_arr[i-1], 0) != -1)
                    usleep(10);

            execvp(line->commands[i].filename, line->commands[i].argv);
            perror("execvp");
            exit(EXIT_FAILURE);
        }
        else if (current_pid == -1){
            fprintf(stderr, "Forking for child command %d failed\n", i+1);
            return EXIT_FAILURE;
        }
        else {fg_forks_pids_arr[i] = current_pid;}
    }
    for(i = 0; i < N_PIPES; i++) fully_close_pipe(pipes_arr[i]);
    
    if( ! line->background)
    {
        for(fg_awaited_child_cmd_i = 0; fg_awaited_child_cmd_i < fg_n_commands; fg_awaited_child_cmd_i++)
        {
            if( ! fg_execution_cancelled )
                waitpid(fg_forks_pids_arr[fg_awaited_child_cmd_i], &ch_status, 0);
            else
                waitpid(fg_forks_pids_arr[fg_awaited_child_cmd_i], NULL, WNOHANG);
            
            if(!fg_execution_cancelled && exec_exit_status == 0 && WIFEXITED(ch_status))
            {
                exec_exit_status = WEXITSTATUS(ch_status);
            }

            if(exec_exit_status != 0 && !fg_execution_cancelled)
                stop_foreground_execution(SIGINT);

            if (fg_awaited_child_cmd_i < N_PIPES) free(pipes_arr[fg_awaited_child_cmd_i]);

            //fprintf(stderr,"%d died\n", fg_awaited_child_cmd_i);fflush(stderr);//DEBUG
        }
        free(pipes_arr); 
        if(!fg_execution_cancelled)
            free(fg_forks_pids_arr);
        
        return exec_exit_status;
    }
    else{
        args = malloc(sizeof(AddJobArgs));
        args->children_pids_arr = fg_forks_pids_arr;
        deep_copy_line(&args->line, line);
        args->used_pipes_arr = pipes_arr;
        
        if(pthread_create(&placeholder, NULL, async_add_bg_job_and_cleanup_after_it, (void*)args) != 0)
        {
            fprintf(stderr, "couldn't create thread for background job\n"); return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
}
// imprime msh> y pide input después de cada enter o comando de fg finalizado
int main()
{
    char buf[BUFFER_SIZE]; char cwd[BUFFER_SIZE];
    pthread_mutex_init(&reading_or_modifying_bg_jobs_mtx, NULL);
    signal(SIGINT, stop_foreground_execution);
    foreground_thread = pthread_self();
    if (getcwd(cwd, sizeof(cwd)) == NULL) {perror("getcwd");exit(EXIT_FAILURE);} 
    printf("msh %s> ", cwd);	
    while (fgets(buf, sizeof(buf), stdin)) 
    {
        run_line(tokenize(buf));    
        fg_execution_cancelled = false;
        fg_n_commands = 0;
        
        if (getcwd(cwd, sizeof(cwd)) == NULL) {perror("getcwd");exit(EXIT_FAILURE);}
        printf("msh %s> ", cwd);
    }
    exit(EXIT_SUCCESS);
}
