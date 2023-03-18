Student Information
-------------------
Evan Lee
Josh Murpy

How to execute the shell
------------------------
Move to the src directory and run "make" first to compile
the program. If "make" pass, run "./cush" to execute the shell.
If executed, any implemented command can be run and perform.


Important Notes
---------------
<Any important notes about your system>
    Our run_command() function will run commands based on the parsing result done in main.
run_command() function will cover all the built ins and non-built ins. This is where 
all the pipe linining, process group, and posix_spawnp() is executed.

    is_builtin() function determines if command line is a built in or not
if command line is a built in, corresponding functionality was implemented.Return 0 if built in
and return 1 if not built in

    find_job() function finds the corresponding job based on given pid

    Our clean_joblist() goes over all the jobs in the joblist and find the jobs that has no process
alive. When we find them we save them an array and we delete all of them at the end.




Description of Base Functionality
---------------------------------
Jobs
    Our is_builtin() function will check if given command line is built in
    if the first command was "jobs", the jobs built in funtion will be performed
    for "jobs" we interate through job list and print all the jobs using print_job() function

fg
    if the first command was "fg", the fg functionality will be run.
    For "fg" first we convert pid in string format to int by using atoi(). After getting the pid,
    we use get_job_from_jid() to find the job that corresponds to jid. Then, we print the job's pipe
    and change the status of job to FOREGROUND. We first check if the job's saved_tty_state was
    changed or not and if not changed we give terminal to the job that was found before by using NULL 
    as first argument because when we assign ownership of the terminal to a process
    group for the first time, we have to call this function with NULL as pg_tty_state.
    If saved_tty_state was changed we use &job1->saved_tty_state instead of NULL.
    After this we send singal to continue this process group and we wait for this job to finish.
    After the job is finished give terminal back to shell.

bg
    if the first command was "bg", the bg functionality will be run.
    For "bg"  first we convert pid in string format to int by using atoi(). After getting the pid,
    we use get_job_from_jid() to find the job that corresponds to jid. Then, we print the job's pipe
    and change the status of job to BACKGROUND and send signal to this process group to continue.

Kill
    if the first command was "kill", the kill functionality will be run. For "kill"  first we 
    convert pid in string format to int by using atoi(). After getting the pid,
    we use get_job_from_jid() to find the job that corresponds to jid. If no corresponding job was found
    print "the process was not killed" and if found send SIGTERM signal to this process groung.
    And our handle_child_status function will get the signal print "terminated" since the signal was
    SIGTERM and decrement the job's number of process alive.

Stop
    if the first command was "stop", the bg functionality will be run.
    For "stop"  first we convert pid in string format to int by using atoi(). After getting the pid,
    we use get_job_from_jid() to find the job that corresponds to jid. We send SIGSTOP signal to this
    process group id. Our handle_child_status will ge the WIFSTOPPED and check if signal is
    SIGSTOP and if so change the job's status to STOPPED and save the terminal state for the futrue
    access and chnge that field in the job structure that indicates the terminal state has changed.


^C
    ^C send SIGINT signal and our cush.c deal with that signal by deleting that job from the job list
    and give terminal back to the shell.


^Z
    ^Z creates SIGTSTP, and out handle_child_status handle this signal by changing the status
    of job to STOPPED and print the current job status. And the current terminal setting is
    saved for the future use of this job then we indicated the saved_tty_state for this jobs
    changed.





Description of Extended Functionality
-------------------------------------

I/O redirection
    We first checked if iored_input, iored_output is NULL or not. If not NULL, we used posix_spawn_file_actions_addopen
    to redirect the input and output. Also we deal with appending to output and duplicating stderr to stdout
    when iored output is not null. For duplicating stderr to stdout we used posix_spawn)posix_spawn_file_actions_adddup2

Pipe
    We first checked if there are more than one command in the pipe line. If so that means we need to create pipeline.
    If n is number of commands ne need n - 1 number of pipes. We created pipeline for every command but the last one so
    that our number of piepline can be n - 1. We used posix_spawn_file_actions_adddup2() to create a pipeline.

Exclusive excess
    We give exclusive access to child process when they are need for command like "nano" and "vim" and when those
    processes are terminated we give terminal back to shell. 




List of Additional Builtins Implemented
---------------------------------------
cd
    We first check to see if the cd command only has 1 arguments. If this is the case we change the directory to the home.
    If this is not the case we change the directory based on the second argument, which will specify the path to the new 
    directory.

history
    The first thing we did was to set up the history struct at the begining of main. After that we need to check if the
    command lines first arguement is an event discriptor. If this is the case the history_expand method will return command 
    to an initiallized char*. We then add this char* to the history using the add_history() method. If the following is not 
    an event disciptor we then call the history builitin. This builtin iterates through the array in the HISTORY_STATE struct
    and print it to the terminal.

