#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "cutil.h"
#define WARP_SIZE 32

__global__ void simple_lock(int* a, int * mutex)
{
    while(atomicCAS(mutex, 0, 1)!=0)
    ;
    a[1]=a[1]+1;
    atomicExch(mutex, 0);
    __syncthreads();
}

int main()
{
    // allocate host copy:
    int* a = (int*)(calloc(WARP_SIZE, sizeof(int)));
    int* mutex = (int*)(calloc(1,sizeof(int)));

    // and device copy
    int* d_a;
    int *d_mutex;
    
    // Build keys
    for(unsigned i=0; i<WARP_SIZE; i++) {
        a[i] = 1;
    }

    CUDA_SAFE_CALL( cudaMalloc( (void**) &d_a, sizeof(int)*WARP_SIZE));
    CUDA_SAFE_CALL( cudaMemcpy( d_a, a, sizeof(int)*WARP_SIZE, cudaMemcpyHostToDevice) );

    CUDA_SAFE_CALL( cudaMalloc( (void**) &d_mutex, sizeof(int)));
    CUDA_SAFE_CALL( cudaMemcpy( d_mutex, mutex, sizeof(int), cudaMemcpyHostToDevice) );

    simple_lock<<<1, WARP_SIZE>>>(d_a, d_mutex);
    
    CUDA_SAFE_CALL( cudaThreadSynchronize() );
    //Copy result from device to host
    CUDA_SAFE_CALL( cudaMemcpy(a ,d_a, sizeof(int)*WARP_SIZE,cudaMemcpyDeviceToHost) );

    for( unsigned int i = 0; i < (WARP_SIZE/10); ++i)
    {   
        for(unsigned int j=0; j < 10; j++)
                printf("%u ",a[i*10+j]);
        printf("\n");
    }
	
}
