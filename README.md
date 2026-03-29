# T20 World Cup Cricket Simulator

A multithreaded cricket simulator built using C and POSIX threads to demonstrate Operating System concepts such as concurrency, synchronization, and scheduling.

## Features

- Multithreaded player simulation
- Mutex protected shared resources
- Semaphore controlled crease capacity
- Condition variables for fielder wake-up
- Round Robin bowler scheduler
- Ball-by-ball match logging
- Gantt chart event logging
- Novelty: ***Match Intensity Parameter***

## Project Structure

code/
yofinal.cpp


## Compilation

```g++ -O2 -pthread -lm -o grandfinale  ./yofinal.cpp```

## Run

```./yofinal```

## Output

- Ball-by-ball match commentary
- Match log file
- Gantt chart log file
