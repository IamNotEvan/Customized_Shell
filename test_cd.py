#!/usr/bin/python
#
# bg_test: tests the bg command
# 
# Test the stop command for stopping a process by its pid.
# Requires the following commands to be implemented
# or otherwise usable:
#
#	Tests the cd custom builtin
#

import sys, imp, atexit, pexpect, proc_check, signal, time, threading
from testutils import *


setup_tests()

# Moving one directory forward
sendline("mkdir test_Directory")
sendline("cd test_Directory")
sendline("pwd")
expect("test_Directory")

# Moving one directory backwards
sendline("cd ..")
sendline("pwd")
expect("home")

# Going to home directory with cd
sendline("cd test_Directory")
sendline("mkdir test_Sub-directory")
sendline("cd")
sendline("pwd")
expect("home")

# Moving two directories at a time
sendline("cd test_Directory/test_Sub-directory/")
sendline("pwd")
expect("test_Sub-directory")

# Failed to cd due to directory not in path
sendline("cd test_Directory/hello/")
expect("Failed to change directory")


test_success()

