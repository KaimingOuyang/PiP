#!/bin/sh

. ../test.sh.inc

$MCEXEC ./getaddr 2>&1 | test_msg_count 'Hello, I am just fine !!'
