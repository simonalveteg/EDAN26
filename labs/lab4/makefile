main:
	gcc -o preflow preflow_barrier_atomic_cp.c pthread_barrier.c -g -O3 -pthread
	time sh check-solution.sh ./preflow
	@echo PASS all tests
