main:
	gcc -std=gnu18 -o preflow preflow_barrier_cp.c pthread_barrier.c -g -O3 -pthread
	time sh check-solution.sh ./preflow
	@echo PASS all tests
