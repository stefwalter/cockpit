# Fuzzing

This branch and directory help do fuzzing on Cockpit. We use AFL for this.

## Preparation

    $ cd fuzzing
    $ CC=/usr/bin/afl-gcc CXX=afl-g++ ../autogen.sh --prefix=/usr --enable-coverage --enable-debug
    $ make -j 8
    $ sudo /bin/sh -c "echo core >/proc/sys/kernel/core_pattern"
    $ sudo /bin/sh -c "cd /sys/devices/system/cpu && echo performance | tee cpu*/cpufreq/scaling_governor"

The option for ```--enable-coverage``` adds a slowdown of about 10%

## HTTP Request Fuzzing

    $ cd fuzzing/
    $ CASES="./http-cases"
    $ afl-fuzz -i $CASES -o $CASES/fuzzer1 -m 1000 ./cockpit-ws --port 0 --request @@

## cockpit-session Fuzzing

To modify the session cases, change stuff in the session-cases/messages directory
and rerun session-cases/messages/prepare.py

You need to first disable PAM delays to make this work.

 * Set 'FAIL_DELAY 0' in /etc/login.defs
 * Add 'nodelay' to pam_unix line in /etc/pam.d/password-auth
 * Remove 'pam_faildelay' line from /etc/pam.d/password-auth

Now you can start with the fuzzing

    $ cd fuzzing/
    $ CASES="./session-cases"
    $ afl-fuzz -i $CASES -o $CASES/fuzzer1 -m 1000 ./cockpit-session unused

## Parallel Fuzzing

    $ cd fuzzing/
    $ CASES="./http-cases"
    $ COMMAND="../cockpit-ws --port 0 --request @@"
    $ afl-fuzz -i $CASES -o $CASES -m 1000 -M fuzzer1 $COMMAND
    $ afl-fuzz -i $CASES -o $CASES -m 1000 -S fuzzer2 $COMMAND
    ...

## Looking at fuzzing coverage

    $ sudo yum install lcov
    $ cd fuzzing/
    $ find . -name *.gcda -delete
    $ rm -rf ./coverage/

    ... now run fuzzing ...

    $ lcov --directory . --capture --output-file coverage.fuzz
    $ genhtml --output-directory ./coverage ./coverage.fuzz
    $ firefox ./coverage/index.html
