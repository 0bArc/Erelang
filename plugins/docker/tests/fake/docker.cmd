@echo off
rem Stand-in for the docker CLI used by the plugin tests. Logs every call, prints canned output.
echo %*>> "%FAKE_DOCKER_LOG%"
if "%1"=="--version" ( echo Docker version 99.0.0, build fake & exit /b 0 )
if "%1"=="info" ( echo 99.0.0 & exit /b 0 )
if "%1"=="version" ( echo 99.0.0 & exit /b 0 )
if "%1"=="ps" goto ps
if "%1"=="images" ( echo {"Repository":"nginx","Tag":"latest","ID":"sha256:abc"} & exit /b 0 )
if "%1"=="run" ( echo c0ffee123 & exit /b 0 )
if "%1"=="stop" ( echo %2 & exit /b 0 )
if "%1"=="start" ( echo %2 & exit /b 0 )
if "%1"=="rm" ( echo removed & exit /b 0 )
if "%1"=="logs" ( echo line one& echo line two & exit /b 0 )
if "%1"=="exec" ( echo exec-output & exit /b 0 )
if "%1"=="inspect" goto inspect
if "%1"=="compose" ( echo compose %2 & exit /b 0 )
echo unknown command %1 1>&2
exit /b 1

:ps
if "%2"=="--format" ( echo web& echo db & exit /b 0 )
echo {"ID":"c0ffee123","Names":"web","Image":"nginx","Status":"Up 2 minutes"}
echo {"ID":"deadbeef","Names":"db","Image":"postgres","Status":"Up 5 minutes"}
exit /b 0

:inspect
if "%2"=="--format" ( echo running & exit /b 0 )
if "%2"=="missing" ( echo Error: No such object: missing 1>&2 & exit /b 1 )
echo [{"Id":"c0ffee123","Name":"/web","State":{"Status":"running","Running":true,"Pid":4242},"Config":{"Image":"nginx"}}]
exit /b 0
