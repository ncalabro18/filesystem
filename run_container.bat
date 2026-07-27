@echo off

setlocal

docker image inspect sfs-riscv >nul 2>&1

if errorlevel 1 (
    docker build -t sfs-riscv .
    if errorlevel 1 exit /b 1
)

docker run --rm -it sfs-riscv bash

pause