@echo off
setlocal
set "BB=%~1"
if "%BB%"=="" set "BB=busybox.exe"
set "T=musl-nt-busybox-test"
set "NTUNIX_TEST=environment-ok"

if exist "%T%" "%BB%" rm -rf "%T%"
"%BB%" mkdir "%T%" || goto fail
"%BB%" printf "gamma\nalpha\nbeta\nbeta\n" > "%T%\source.txt" || goto fail
"%BB%" cat "%T%/source.txt" || goto fail
"%BB%" basename /alpha/beta || goto fail
"%BB%" dirname /alpha/beta || goto fail
"%BB%" expr 6 * 7 || goto fail
"%BB%" seq 3 || goto fail
"%BB%" printenv NTUNIX_TEST || goto fail
"%BB%" cp "%T%/source.txt" "%T%/copy.txt" || goto fail
"%BB%" chmod 640 "%T%/copy.txt" || goto fail
"%BB%" chgrp 0 "%T%/copy.txt" || goto fail
"%BB%" chown 0:0 "%T%/copy.txt" || goto fail
"%BB%" touch "%T%/copy.txt" || goto fail
"%BB%" stat -c "stat:size=%%s mode=%%a" "%T%/copy.txt" || goto fail
"%BB%" ls -la "%T%" || goto fail
"%BB%" du -k "%T%" || goto fail
"%BB%" df -k || goto fail
"%BB%" realpath "%T%/copy.txt" || goto fail
"%BB%" head -n 2 "%T%/source.txt" || goto fail
"%BB%" tail -n 2 "%T%/source.txt" || goto fail
"%BB%" wc "%T%/source.txt" || goto fail
"%BB%" cut -c 1-3 "%T%/source.txt" || goto fail
"%BB%" tr a-z A-Z < "%T%\source.txt" || goto fail
"%BB%" sort "%T%/source.txt" > "%T%\sorted.txt" || goto fail
"%BB%" uniq "%T%/sorted.txt" || goto fail
"%BB%" comm "%T%/sorted.txt" "%T%/sorted.txt" || goto fail
"%BB%" nl "%T%/source.txt" || goto fail
"%BB%" paste "%T%/source.txt" "%T%/source.txt" || goto fail
"%BB%" fold -w 3 "%T%/source.txt" || goto fail
"%BB%" tac "%T%/source.txt" || goto fail
"%BB%" tee "%T%/tee.txt" < "%T%\source.txt" || goto fail
"%BB%" cksum "%T%/source.txt" || goto fail
"%BB%" crc32 "%T%/source.txt" || goto fail
"%BB%" sum "%T%/source.txt" || goto fail
"%BB%" md5sum "%T%/source.txt" || goto fail
"%BB%" sha256sum "%T%/source.txt" || goto fail
"%BB%" base32 "%T%/source.txt" || goto fail
"%BB%" base64 "%T%/source.txt" || goto fail
"%BB%" od -An -tx1 "%T%/source.txt" || goto fail
"%BB%" dd if="%T%/source.txt" of="%T%/dd.txt" bs=4 status=none || goto fail
"%BB%" cp "%T%/source.txt" "%T%/dos.txt" || goto fail
"%BB%" unix2dos "%T%/dos.txt" || goto fail
"%BB%" dos2unix "%T%/dos.txt" || goto fail
"%BB%" install -m 644 "%T%/source.txt" "%T%/installed.txt" || goto fail
"%BB%" mktemp "%T%/temporary.XXXXXX" || goto fail
"%BB%" split -l 2 "%T%/source.txt" "%T%/part-" || goto fail
"%BB%" truncate -s 5 "%T%/copy.txt" || goto fail
"%BB%" fsync "%T%/copy.txt" || goto fail
"%BB%" mv "%T%/copy.txt" "%T%/moved.txt" || goto fail
"%BB%" ln "%T%/moved.txt" "%T%/hardlink.txt" || goto fail
"%BB%" link "%T%/moved.txt" "%T%/hardlink2.txt" || goto fail
"%BB%" stat -c "link:size=%%s links=%%h" "%T%/hardlink.txt" || goto fail
"%BB%" mkdir "%T%/empty" || goto fail
"%BB%" rmdir "%T%/empty" || goto fail
"%BB%" unlink "%T%/hardlink2.txt" || goto fail
"%BB%" sleep 0.01 || goto fail
"%BB%" usleep 1000 || goto fail
"%BB%" date -u || goto fail
"%BB%" uname -a || goto fail
"%BB%" arch || goto fail
"%BB%" hostid || goto fail
"%BB%" nproc || goto fail
"%BB%" id || goto fail
"%BB%" groups || goto fail
"%BB%" whoami || goto fail
"%BB%" sync || goto fail
"%BB%" rm -rf "%T%" || goto fail
"%BB%" test ! -e "%T%" || goto fail
"%BB%" echo "busybox-runtime: PASS"
exit /b 0

:fail
echo busybox-runtime: FAIL, errorlevel=%errorlevel%
if exist "%T%" "%BB%" rm -rf "%T%"
exit /b 1
