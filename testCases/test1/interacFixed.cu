// includes, system
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cutil.h"


// #define DEBUG
// Only one of these should be enabled at a time. MKaech
//#define TM_SYNC
#define ATOMIC_SYNC
//#define NO_SYNC

#define NUM_ACCOUNTS 1000000
#define NUM_TRANSACTIONS 122880

#define THREADS_PER_BLOCK_X 192
#define THREADS_PER_BLOCK_Y 1
#define THREADS_PER_BLOCK_Z 1

#define BLOCKS_PER_GRID_X	120
#define BLOCKS_PER_GRID_Y	1
#define BLOCKS_PER_GRID_Z	1	// As of CUDA 2.0 this dimension MUST be 1.  MKaech

#define THREADS_PER_BLOCK	(THREADS_PER_BLOCK_X * THREADS_PER_BLOCK_Y * THREADS_PER_BLOCK_Z)
#define TOTAL_THREADS		(THREADS_PER_BLOCK * BLOCKS_PER_GRID_X * BLOCKS_PER_GRID_Y * BLOCKS_PER_GRID_Z)

// these macros are for use in the shader!
#define BLOCK_ID			( blockIdx.x + (BLOCKS_PER_GRID_X * blockIdx.y) + (BLOCKS_PER_GRID_X * BLOCKS_PER_GRID_Y * blockIdx.z) )
#define THREAD_ID			( (THREADS_PER_BLOCK * BLOCK_ID) + threadIdx.x + (THREADS_PER_BLOCK_X * threadIdx.y) + (THREADS_PER_BLOCK_X * THREADS_PER_BLOCK_Y * threadIdx.z) )

struct account
{
	int lock;
	int balance;
};

struct transaction
{
	int amount;
	int src_account;
	int dest_account;
};



__global__ void interac_atomic( account* __restrict__ accounts, transaction *__restrict__ transactions, int numTransactions)
{
	int id = THREAD_ID; 
	for(int index = id; index < numTransactions; index += TOTAL_THREADS)
	{
		transaction* action = &transactions[index];
		account* src = &accounts[action->src_account];
		account* dest = &accounts[action->dest_account];
		
		// sanity check
		if(action->src_account == action->dest_account)
		{
			continue;
		}
	
		// acquire locks
      		account* lock1;
      		account* lock2; 
      		if (src > dest) {
        		lock1 = src; 
         		lock2 = dest;
      		} else {
         		lock2 = src; 
         		lock1 = dest;
      		}
	
    		// do transaction
               int done = 0;
               // do{
                while(!done){
                    if(atomicCAS(&lock1->lock, 0, 1) == 0){
                        if(atomicCAS(&lock2->lock,0,1) ==0){
               	      	// do transaction
              		    src->balance -= action->amount;
               		    dest->balance += action->amount;
    
               		    // release locks
               	    	atomicExch(&lock2->lock, 0);
               		    atomicExch(&lock1->lock, 0);

			            done=1;
				    continue;
                     }
                     atomicExch(&lock1->lock,0);
                  }
			// atomicExch(&lock1->lock, 0);
                }//while(!done);

	}
}

void interac_gold(account* __restrict__  accounts, transaction* __restrict__ transactions, int num_transactions)
{
	for(int i = 0; i < num_transactions; ++i)
	{
		transaction* action = &transactions[i];
		account* src = &accounts[action->src_account];
		account* dest = &accounts[action->dest_account];
		
		src->balance -= action->amount;
		dest->balance += action->amount;
	}
}
	
int main(int argc, const char** argv)
{
    printf("Initializing...\n");
    //CUT_DEVICE_INIT(argc, argv);
    cudaSetDevice(0);
    cudaDeviceProp prop;
    int d=0;
    cudaGetDevice(&d); 
    cudaGetDeviceProperties(&prop,d);
    printf("Device Number: %d\n", d);
    printf("  Device name: %s\n", prop.name);

    bool useTM = false;
    srand(2009);  // set seed for rand()

    // allocate host memory for accounts
    unsigned int accounts_size = sizeof(account) * NUM_ACCOUNTS;
	unsigned int transactions_size = sizeof(transaction) * NUM_TRANSACTIONS;
    account* host_accounts = (account*)malloc(accounts_size);
	account* gold_accounts = (account*)malloc(accounts_size);
	transaction* host_transactions = (transaction*)malloc(transactions_size);

	// create random account balances
    for (int i = 0; i < NUM_ACCOUNTS; ++i)
	{
		host_accounts[i].lock = 0;
        host_accounts[i].balance = (int) fmod((float)rand(),100.0f);
		
		gold_accounts[i].lock = 0;
		gold_accounts[i].balance = host_accounts[i].balance;
#ifdef DEBUG
		printf( "acct%u : $%d\n", i, host_accounts[i].balance );
#endif
	}
	
	// create random transaction pairs
	for (int i = 0; i < NUM_TRANSACTIONS; ++i)
	{
		host_transactions[i].amount = (int) fmod((float)rand(),50.0f);
		host_transactions[i].src_account = rand() % NUM_ACCOUNTS;	
		host_transactions[i].dest_account = rand() % NUM_ACCOUNTS;
#ifdef DEBUG
		printf( "%u : $%d from acct%u => to acct%u\n", 
			i, host_transactions[i].amount, 
			host_transactions[i].src_account, 
			host_transactions[i].dest_account );
#endif		
		// make sure src != dest
		while(host_transactions[i].src_account == host_transactions[i].dest_account)
		{
			host_transactions[i].dest_account = rand() % NUM_ACCOUNTS;
		}
	}

    // allocate device memory
    account* device_accounts;
	transaction* device_transactions;
    CUDA_SAFE_CALL(cudaMalloc((void**) &device_accounts, accounts_size));
    CUDA_SAFE_CALL(cudaMalloc((void**) &device_transactions, transactions_size));

    // copy host memory to device
    CUDA_SAFE_CALL(cudaMemcpy(device_accounts, host_accounts, accounts_size, cudaMemcpyHostToDevice) );
    CUDA_SAFE_CALL(cudaMemcpy(device_transactions, host_transactions, transactions_size, cudaMemcpyHostToDevice) );
    
    // setup execution parameters
	dim3 block_size(THREADS_PER_BLOCK_X, THREADS_PER_BLOCK_Y, THREADS_PER_BLOCK_Z);
	dim3 grid_size(BLOCKS_PER_GRID_X, BLOCKS_PER_GRID_Y, BLOCKS_PER_GRID_Z);
	
	printf("Beginning kernel execution...\n");
	
    // create and start timer
    unsigned int timer = 0;
    //CUT_SAFE_CALL(cutCreateTimer(&timer));
    //CUT_SAFE_CALL(cutStartTimer(timer));

    // execute the kernel
    interac_atomic<<< grid_size, block_size >>>(device_accounts, device_transactions, NUM_TRANSACTIONS);
	
    cudaThreadSynchronize();

    // check if kernel execution generated and error
    CUT_CHECK_ERROR("Kernel execution failed");

    // copy result from device to host
    CUDA_SAFE_CALL(cudaMemcpy(host_accounts, device_accounts, accounts_size, cudaMemcpyDeviceToHost) );
	
    // stop and destroy timer
    //CUT_SAFE_CALL(cutStopTimer(timer));
    //printf("Kernel processing time: %f (ms) \n", cutGetTimerValue(timer));
    //CUT_SAFE_CALL(cutDeleteTimer(timer));
	
	printf("Computing gold results...\n");
	
    unsigned int timer_cpu = 0;
    //CUT_SAFE_CALL(cutCreateTimer(&timer_cpu));
    //CUT_SAFE_CALL(cutStartTimer(timer_cpu));
    interac_gold(gold_accounts, host_transactions, NUM_TRANSACTIONS);
    //CUT_SAFE_CALL(cutStopTimer(timer_cpu));
    //printf("Gold result processing time: %f (ms) \n", cutGetTimerValue(timer_cpu));
    //CUT_SAFE_CALL(cutDeleteTimer(timer_cpu));
	
	printf("Comparing results...\n");

    // check result
	bool success = true;
    for (int i = 0; i < NUM_ACCOUNTS; ++i)
	{
		if(gold_accounts[i].balance != host_accounts[i].balance)
		{
			success = false;
			printf("Difference found in account %d: Gold = %d, Kernel = %d\n", i, gold_accounts[i].balance, host_accounts[i].balance);
		}
	}
	
	printf("Test %s\n", (success ? "PASSED! All account balances were correct." : "FAILED!"));

    // clean up memory
    free(host_accounts);
	free(gold_accounts);
	free(host_transactions);
    CUDA_SAFE_CALL(cudaFree(device_accounts));
	CUDA_SAFE_CALL(cudaFree(device_transactions));
	
    //CUT_EXIT(argc, argv);
}
