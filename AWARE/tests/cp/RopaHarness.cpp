/*
 *  RopaHarness.cpp
 *  Ropa
 *
 *  Created by Andrew on 09-02-12.
 *  Copyright 2009 Electronic Arts, Inc. All rights reserved.
 *
 */

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>

#include "RopaHarness.h"
#include "Effect.h"
#include <vector>

#include <stdarg.h>
#include <GL/glut.h>
#include <CL/cl.h>
#include <CL/cl_gl.h>
#include <string.h>

#include <fstream>
#include <iostream>

#include <unistd.h>

#define HAS_GL_DEVICE 0 
#define CL_GL_INTEROP_WORKING 0
#define USE_ONLY_CPU_DEVICES 0
#define USE_ONLY_GPU_DEVICES 0

#define USE_CL_INTEGRATOR 1
#define USE_CL_WRITEBACK 1
#define USE_CL_DISTANCE 1
#define USE_CL_DRIVER 1

#define INDIVIDUAL_KERNEL_TIMING 0

#if INDIVIDUAL_KERNEL_TIMING
#include <mach/mach_time.h>
#include <Carbon/Carbon.h>
#endif

class CLBuffer;
class CLKernel;
class CLProgram;
class CLCmdQueue;

const int kMaxDevices = 2;
const int kNumKernelModes = 4;	// task, scalar, vector, vectask
const int kMaxKernels = 32;	// integrator, writeback, constraints x 16
const char* kKernelModeNames[4] = { "task", "scalar", "vector", "vectask" };

const int kComputeUnitMultiplier = 4;

void Print_Memory(unsigned char *display ,int length)
{
    char filename[100];
	printf("\n Enter File Name: ");
	scanf("%s",filename);

	FILE  *myfile;
	myfile = fopen(filename,"w");
 
	fprintf(myfile,"\n======================================================================================\n");
   //create row, col, and i.  Set i to 0
    int counter=0 , i = 0;
   //iterate through the rows,
   for(i = 0; i < length; i++)
    {
       //print hex representation

           if(i<length)
               fprintf(myfile,"%02X", display[i]);
           //print a blank if the current index is past the end
           else
                fprintf(myfile,"  ");

            //print a space to keep the values separate
            fprintf(myfile," ");

            counter++;
         if(counter > 16)
         {//create a new row
            fprintf(myfile,"\n");
            counter=0;
         }
   }
   fprintf(myfile,"\n======================================================================================\n");
   fclose(myfile);
}

void Print_Memory2(const char * filename, const char * mode, unsigned char *display ,int length)
{

	FILE  *myfile;
	myfile = fopen(filename, mode);

	fprintf(myfile,"\n======================================================================================\n");
   //create row, col, and i.  Set i to 0
    int counter=0 , i = 0;
   //iterate through the rows,
   for(i = 0; i < length; i++)
    {
       //print hex representation

           if(i<length)
               fprintf(myfile,"%02X", display[i]);
           //print a blank if the current index is past the end
           else
                fprintf(myfile,"  ");

            //print a space to keep the values separate
            fprintf(myfile," ");

            counter++;
         if(counter > 16)
         {//create a new row
            fprintf(myfile,"\n");
            counter=0;
         }
   }
   fprintf(myfile,"\n======================================================================================\n");
   fclose(myfile);
}

void Print_Memory_ints(const char * filename, const char * mode, int *display ,int length)
{
   FILE  *myfile;
   myfile = fopen(filename, mode);

   for(unsigned i = 0; i < length; i ++)
   {
      fprintf(myfile, "%d ", display[i]);
   }
   fclose(myfile);
}


extern "C" 
{
	void CLBreakpoint (int b)
	{
		printf("CLBreakpoint %d\n", b);
	}
	
	void CLPrintf(const char* fmt, ...)
	{
		va_list vl;
		va_start(vl, fmt);
		printf("CLPrintf: ");
		vprintf(fmt, vl);
		va_end(vl);
	}
};

static recVec gOrigin = {0.0, 0.0, 0.0};

#define EVENTID_PRINTS 0
void EventID::PrintPre (const char* msg)
{
#if EVENTID_PRINTS
	if (mCLPtr != NULL)
	{
		fprintf(stderr, "EventID::%-16s this=%p ", msg, mCLPtr);

		cl_uint refcount;
		clGetEventInfo((cl_event)mCLPtr, CL_EVENT_REFERENCE_COUNT, sizeof(refcount), &refcount, NULL);
		fprintf(stderr, " pre=%u ", refcount);
	}
#endif
}

void EventID::PrintPost ()
{
#if EVENTID_PRINTS
	if (mCLPtr != NULL)
	{
		cl_uint refcount;
		clGetEventInfo((cl_event)mCLPtr, CL_EVENT_REFERENCE_COUNT, sizeof(refcount), &refcount, NULL);
		fprintf(stderr, " post=%u\n", refcount);
	}
#endif
}

void EventID::Retain ()
{
	if (mCLPtr != NULL)
	{
		this->PrintPre("Retain");
		clRetainEvent((cl_event)mCLPtr);
		this->PrintPost();
	}
}

void EventID::Release ()
{
	if (mCLPtr != NULL)
	{
		this->PrintPre("Release");
		clReleaseEvent((cl_event)mCLPtr);
		this->PrintPost();
		mCLPtr = NULL;
	}
}

void EventID::Wait ()
{
	if (mCLPtr != NULL)
	{
		this->PrintPre("Wait");

		cl_event e = (cl_event)mCLPtr;
		clWaitForEvents(1, &e);
		
		this->PrintPost() ;
	}
}


struct RopaConstructionData
{
	std::vector<Vector4>			mReplications;
	
	std::vector<ROPA::Particle>		mParticles;
	std::vector<ROPA::PosNormPair>	mDrivers;
	std::vector<ROPA::VolumeTransform>	mVolumes;
	std::vector<ROPA::ConstraintOctet>	mOctets[16];
	std::vector<Matrix44>			mFramePositions;
	std::vector<float>				mFrameTimes;
	std::vector<uint16_t>			mVertexMap;
	std::vector<uint32_t>			mMappedVertices;
	std::vector<uint16_t>			mReverseMap;
	std::vector<uint16_t>			mVertIndices;
	
	void PerformReplications ()
	{
		int numreps = mReplications.size();
		if (numreps == 0) return;
		printf("%d replications\n", numreps);

		while (mDrivers.size() > mParticles.size())
			mDrivers.pop_back();
		
		int numparticles = mParticles.size();
		int numdrivers = mDrivers.size();
		int numverts = mReverseMap.size();
		int numindices = mVertIndices.size();
		
//		int vertexmapsize = mVertexMap.size();
//		int mappedsize = mMappedVertices.size();
		
		int numOctets[16];
		for (int o = 0; o < 16; ++o)
			numOctets[o] = mOctets[o].size();
		
		for (int r = 0; r < numreps; ++r)
		{
			for (int p = 0; p < numparticles; ++p)
			{
				ROPA::Particle particle = mParticles[p];
				particle.mPos += mReplications[r];
				particle.mPrevPos += mReplications[r];
				mParticles.push_back(particle);
				mVertexMap.push_back(mVertexMap[p] + numverts * (r+1));
			}
			for (int d = 0; d < numdrivers; ++d)
			{
				ROPA::PosNormPair driver = mDrivers[d];
				driver.mPosition += mReplications[r];
				mDrivers.push_back(driver);
			}
			for (int v = 0; v < numverts; ++v)
			{
				mReverseMap.push_back(mReverseMap[v] + numparticles * (r+1));
			}
			for (int i = 0; i < numindices; ++i)
			{
				mVertIndices.push_back(mVertIndices[i+0] + numverts * (r+1));
			}
			
			for (int o = 0; o < 16; ++o)
			{
				for (int n = 0; n < numOctets[o]; ++n)
				{
					ROPA::ConstraintOctet co = mOctets[o][n];
					for (int s = 0; s < 8; ++s)
					{
						co.mRefObjIdx[s] += numdrivers * (r+1);
						co.mParticleIdx[s] += numparticles * (r+1);
					}
					mOctets[o].push_back(co);
				}
			}
		}
		
		// we interleave so that octets which can execute in parallel are located together
		for (int o = 0; o < 16; ++o)
		{
			std::vector<ROPA::ConstraintOctet> temp(mOctets[o]);
			mOctets[o].clear();
			for (int n = 0; n < numOctets[o]; ++n)
			{
				ROPA::ConstraintOctet co = temp[n];
				mOctets[o].push_back(co);
				for (int rep = 0; rep < numreps; ++rep)
				{
					int index = (rep+1)*numOctets[o]+n;
					co = temp[index];
					mOctets[o].push_back(co);
				}
			}
		}		
		
		int nummapped = (numverts*numreps+31)/32;
		unsigned int allbitson = ~0U;
		for (int m=0; m<mMappedVertices.size(); ++m)
			mMappedVertices[m] = allbitson;
		while (mMappedVertices.size() < nummapped)
			mMappedVertices.push_back(allbitson);
	}
};

static void clNotifyError (const char* errinfo, const void* private_info, size_t cb, void *user_data)
{
	printf("OpenCL Error Notification:  %s\n", errinfo);
}

class CLBuffer
{
public:
	cl_mem	mBuffer;
	void*	mSource;
	size_t	mBytes;
	
	CLBuffer () : mBuffer(0), mSource(NULL), mBytes(0) {}
	~CLBuffer () { if (mBuffer != 0) clReleaseMemObject(mBuffer); }
	
	void Create (cl_context context, void* data, size_t bytes, cl_mem_flags flags)
	{
		cl_int err;
		mBytes = bytes;
		mSource = data;
		mBuffer = clCreateBuffer(context, flags, bytes, data, &err);
		if (err != CL_SUCCESS)
			printf("Failed to create mem buffer object of size %ld from data at %p and flags %d, err=%d\n", bytes, data, (int)flags, err);
	}
	
	void SetAsArg (cl_kernel k, unsigned int index) const
	{
//		printf("SetAsArg(index=%d, source=%p, bytes=%ld)\n", index, mSource, mBytes);
		
		cl_int err = clSetKernelArg(k, index, sizeof(mBuffer), &mBuffer);
		if (err != CL_SUCCESS)
			printf("SetKernelArg(index=%d,size=%ld) failed with err=%d\n", index, sizeof(mBuffer), err);
	}
	
	void Map (cl_command_queue q) const
	{
		cl_int err;
		void* ptr = clEnqueueMapBuffer(q, mBuffer, true, CL_MAP_READ|CL_MAP_WRITE, 0, mBytes, 0, NULL, NULL, &err);
		if ((ptr == NULL) && (err != CL_SUCCESS))
			printf("CLBuffer::Map failed with err=%d\n", err);
		if (ptr != mSource)
			printf("Map didn't put buffer back where it started!\n");
	}
	
	void Unmap (cl_command_queue q) const
	{
		cl_int err = clEnqueueUnmapMemObject(q, mBuffer, mSource, 0, NULL, NULL);
		if (err != CL_SUCCESS)
			printf("CLBuffer::Unmap failed with err=%d\n", err);
	}
	
	void Release ()
	{
		if (mBuffer != 0)
			clReleaseMemObject(mBuffer);
		mBuffer = 0;
	}
};

class CLGLBuffer
	{
	public:
		cl_mem	mCLBuffer;
		GLuint	mGLBuffer;
		
		CLGLBuffer () : mCLBuffer(0), mGLBuffer(0) {}
		~CLGLBuffer () { if (mCLBuffer != 0) clReleaseMemObject(mCLBuffer); }
		
		void Create (cl_context context, GLuint glb, cl_mem_flags flags)
		{
			cl_int err;
			mGLBuffer = glb;
			mCLBuffer = clCreateFromGLBuffer(context, flags, glb, &err);
			if (err != CL_SUCCESS)
				printf("Failed to create shared CL/GL buffer from GL buffer %d, err=%d\n", glb, err);
		}
		
		void SetAsArg (cl_kernel k, unsigned int index) const
		{
			cl_int err = clSetKernelArg(k, index, sizeof(mCLBuffer), &mCLBuffer);
			if (err != CL_SUCCESS)
				printf("SetKernelArg(index=%d,size=%ld) failed with err=%d\n", index, sizeof(mCLBuffer), err);
		}
		
		void Release ()
		{
			if (mCLBuffer != 0)
				clReleaseMemObject(mCLBuffer);
			mCLBuffer = 0;
			mGLBuffer = 0;
		}
	};

class CLKernel
{
public:
	const char* mTaskName;
	const char* mScalarName;
	const char* mVectorName;
	int			mIndex;
	
	uint64_t	mAccumulatedTime;
	uint64_t	mCalls;
	
	cl_kernel	mTaskKernel;
	cl_kernel	mScalarKernel;
	cl_kernel	mVectorKernel;
	
	CLKernel () : mTaskName(""), mScalarName(""), mVectorName(""), mAccumulatedTime(0), mCalls(0), mTaskKernel(0), mScalarKernel(0), mVectorKernel(0) {}
	~CLKernel () 
	{ 
		if (mTaskKernel != 0) 
			clReleaseKernel(mTaskKernel); 
		if (mScalarKernel != 0) 
			clReleaseKernel(mScalarKernel); 
		if (mVectorKernel != 0) 
			clReleaseKernel(mVectorKernel); 
	}
	
	cl_kernel Get (int kernelmode) const
	{
		switch (kernelmode)
		{
			default:
			case 0:
				return mTaskKernel;
			case 1:
				return mScalarKernel;
			case 2:
			case 3:
				return mVectorKernel;
		}
	}
	
	const char* GetName (int kernelmode) const
	{
		switch (kernelmode)
		{
			default:
			case 0:
				return mTaskName;
			case 1:
				return mScalarName;
			case 2:
			case 3:
				return mVectorName;
		}
	}
	
	void AccumulateTime (uint64_t dt)
	{
		mAccumulatedTime += dt;
		mCalls += 1;
	}
	
	double AverageDuration ()
	{
#if INDIVIDUAL_KERNEL_TIMING
		Nanoseconds elapsednano = AbsoluteToNanoseconds(*(AbsoluteTime*)&mAccumulatedTime);
		uint64_t nanosecs = *(uint64_t*)&elapsednano;
		double nanos_per_call = (double)nanosecs / (double)mCalls;
		mAccumulatedTime = 0;
		mCalls = 0;
		return nanos_per_call * 1e-6;
#else
		return 0.0;
#endif
	}
	
	void Create (cl_program p, int index, const char* task_name, const char* scalar_name, const char* vector_name)
	{
		mTaskName = task_name;
		mScalarName = scalar_name;
		mVectorName = vector_name;
		mIndex = index;
		
		cl_int err;
		mTaskKernel = clCreateKernel(p, task_name, &err);
		if (err != CL_SUCCESS)
			printf("Failed to get kernel %s, err=%d\n", task_name, err);
		
		mScalarKernel = clCreateKernel(p, scalar_name, &err);
		if (err != CL_SUCCESS)
			printf("Failed to get kernel %s, err=%d\n", scalar_name, err);
		
		mVectorKernel = clCreateKernel(p, vector_name, &err);
		if (err != CL_SUCCESS)
			printf("Failed to get kernel %s, err=%d\n", vector_name, err);
	}
};

class CLProgram
{
public:
	cl_program	mProgram;
   DistanceSolverOptions distanceSolverOpts; 
	
	CLProgram () : mProgram(0) {}
	~CLProgram () { if (mProgram != 0) clReleaseProgram(mProgram); }
	
	bool Compile (int numdevices, cl_device_id *devices, const char* options)
	{
		cl_int err = clBuildProgram(mProgram, numdevices, devices, options, NULL, NULL);
		if (err != CL_SUCCESS) 
		{ 
			static char error_buffer[64*1024];
			size_t error_bytes;
			for (int d = 0; d < numdevices; ++d)
			{
				printf("BuildProgram err=%d\n", err);
				
				err = clGetProgramBuildInfo(mProgram, devices[d], CL_PROGRAM_BUILD_LOG, sizeof(error_buffer), error_buffer, &error_bytes);
				if (err != CL_SUCCESS)
					printf("clGetProgramBuildInfo err=%d\n", err);
				
				if (error_bytes == sizeof(error_buffer))
					error_bytes -= 1;
				error_buffer[error_bytes] = 0;
				printf("###################################################################################\nError log for device %d [%p]\n###################################################################################\n%s\n###################################################################################\n", d, devices[d], error_buffer);
			}
			return false;
		}
		else
			return true;
	}
	
	bool Load (cl_context context, int numdevices, cl_device_id *devices, const char* opencl_code, int cpuIndex, int gpuIndex, int otherIndex)
	{
		cl_int err;
		size_t programlength = strlen(opencl_code);
		const char** programsource = (const char**)&opencl_code;
		mProgram = clCreateProgramWithSource(context, 1, programsource, &programlength, &err);
		if (err != CL_SUCCESS) { printf("clCreateProgramWithSource failed with err=%d\n", err); return false; }
		CLPrintf("Testing CLPrintf from host.\n");

		bool success = false;

#if 1
		const char* cpuOptions = "";	//"-Werror -cl-finite-math-only -cl-fast-relaxed-math -cl-unsafe-math-optimizations -cl-no-signed-zeros -cl-mad-enable -cl-single-precision-constant -cl-denorms-are-zero -d ISCPU=1";
		if ((devices[cpuIndex] != 0) && this->Compile(1, &devices[cpuIndex], cpuOptions))
			success = true;
		else
			devices[cpuIndex] = (cl_device_id)0;
		
		const char* gpuOptions;	//"-Werror -cl-finite-math-only -cl-fast-relaxed-math -cl-unsafe-math-optimizations -cl-no-signed-zeros -cl-mad-enable -cl-single-precision-constant -cl-denorms-are-zero -d ISGPU=1";
		if(distanceSolverOpts.useTM)
			if(distanceSolverOpts.useTMOpt)
				gpuOptions = "-D TM -D TMOPT";
			else 
				gpuOptions = "-D TM ";
		else
			gpuOptions = "";

		if ((devices[gpuIndex] != 0) && this->Compile(1, &devices[gpuIndex], gpuOptions))
			success = true;
		else
			devices[gpuIndex] = (cl_device_id)0;
		
//const char* otherOptions = "-Werror -cl-finite-math-only -cl-fast-relaxed-math -cl-unsafe-math-optimizations -cl-no-signed-zeros -cl-mad-enable -cl-single-precision-constant -cl-denorms-are-zero";
//		if ((devices[otherIndex] != 0) && this->Compile(1, &devices[otherIndex], otherOptions))
//			success = true;
//		else
//			devices[otherIndex] = (cl_device_id)0;
#else
		success = this->Compile(numdevices, devices, "");  // "-Werror -cl-finite-math-only -cl-fast-relaxed-math -cl-unsafe-math-optimizations -cl-no-signed-zeros -cl-mad-enable -cl-single-precision-constant -cl-denorms-are-zero");
#endif
		
		//
		// Dump ptx to file
		//
		std::ofstream myfile("ropa.ptx");
		cl_uint program_num_devices;
		clGetProgramInfo(mProgram, CL_PROGRAM_NUM_DEVICES, sizeof(cl_uint), &program_num_devices, NULL);
		if (program_num_devices == 0)
		{
						std::cerr << "no valid binary was found" << std::endl;
						//return;
		}
		size_t binaries_sizes[program_num_devices];
		clGetProgramInfo(mProgram,     CL_PROGRAM_BINARY_SIZES, program_num_devices*sizeof(size_t), binaries_sizes, NULL);
		char **binaries = new char*[program_num_devices];
		for (size_t i = 0; i < program_num_devices; i++)
			binaries[i] = new char[binaries_sizes[i]+1];
		clGetProgramInfo(mProgram, CL_PROGRAM_BINARIES, program_num_devices*sizeof(size_t), binaries, NULL);
		if(myfile.is_open())
		{
			for (size_t i = 0; i < program_num_devices; i++)
			{
				myfile << binaries[i];
			}
		}
		myfile.close();
		// Done dumping ptx to file



		if (!success)
		{
			printf("clBuildProgram failed on all devices\n");
			clReleaseProgram(mProgram); 
			mProgram = 0; 
			return false; 
		}
		else
		{
			printf("CL program compiled on at least one device.\n");
			return true;
		}
	}
};

class CLCmdQueue
{
public:
	cl_command_queue mCmdQ;
	const char* mType;
	
	CLCmdQueue () : mCmdQ(0), mType("<undefined>") {}
	~CLCmdQueue () { if (mCmdQ != 0) clReleaseCommandQueue(mCmdQ); }
	
	void Create (cl_context context, cl_device_id d, const char* type)
	{
		cl_int err;
		mType = type;
		mCmdQ = clCreateCommandQueue(context, d, 0, &err);
		if (err != CL_SUCCESS) { printf("clCreateCommandQueue failed for %s device, error=%d\n", mType, err); }
	}
};

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------

class RopaOpenCL;

struct opencl_RopaIntegrator : public ROPA::IntegratorFunc
{
	RopaOpenCL*	mCLInterface;
	CLKernel	mKernel;
	
	void Init (RopaOpenCL *rcl, int kernelindex);	
	virtual EventID operator() (EventID event, float dt, const Matrix44& deltaPos, const Vector4& gravity, const ROPA::ConstraintTuningParameters& ctp, ROPA::IntegratorState& istate);
};


struct opencl_RopaWriteBack : public ROPA::WriteBackFunc
{
	RopaOpenCL*	mCLInterface;
	Matrix44	mTransformMatrix;
	CLKernel	mKernel;
	
	void Init (RopaOpenCL *rcl, int kernelindex);	
	virtual EventID operator() (EventID event, const Matrix44& mat);
};




struct opencl_GenericSolver : public ROPA::ConstraintSolver
{
	RopaOpenCL*	mCLInterface;
	CLKernel	mKernel;
	void Init (RopaOpenCL* rcl, int kernelindex, const char* tName, const char* sName, const char* vName);
	virtual EventID operator() (EventID event, int constraintIndex, int numOctets, int numIters, short refObjIdx);
};


struct opencl_DistanceSolver : public opencl_GenericSolver
{
	void Init (RopaOpenCL *rcl, int kernelindex) { opencl_GenericSolver::Init(rcl, kernelindex, "task_distancesolver", "scalar_distancesolver", "vector_distancesolver"); }
	void Init_tm (RopaOpenCL *rcl, int kernelindex) { opencl_GenericSolver::Init(rcl, kernelindex, "task_distancesolver", "scalar_distancesolver_tm", "vector_distancesolver"); }
	void Init_atomic (RopaOpenCL *rcl, int kernelindex) { opencl_GenericSolver::Init(rcl, kernelindex, "task_distancesolver", "scalar_distancesolver_atomic", "vector_distancesolver"); }
	//virtual EventID operator() (EventID event, int numOctets, int numIters, short refObjIdx);
};

struct opencl_DriverSolver : public opencl_GenericSolver
{
	void Init (RopaOpenCL *rcl, int kernelindex) { opencl_GenericSolver::Init(rcl, kernelindex, "task_driversolver", "scalar_driversolver", "vector_driversolver"); }
	virtual EventID operator() (EventID event, int constraintIndex, int numOctets, int numIters, short refObjIdx);
};

struct opencl_SphereSolver : public opencl_GenericSolver
{
	void Init (RopaOpenCL *rcl, int kernelindex) { opencl_GenericSolver::Init(rcl, kernelindex, "task_spheresolver", "scalar_spheresolver", "vector_spheresolver"); }
	virtual EventID operator() (EventID event, int constraintIndex, int numOctets, int numIters, short refObjIdx);
};

struct opencl_TubeSolver : public opencl_GenericSolver
{
	void Init (RopaOpenCL *rcl, int kernelindex) { opencl_GenericSolver::Init(rcl, kernelindex, "task_tubesolver", "scalar_tubesolver", "vector_tubesolver"); }
	virtual EventID operator() (EventID event, int constraintIndex, int numOctets, int numIters, short refObjIdx);
};



struct opencl_LockBuffers : public ROPA::LockBuffersFunc
{
	RopaOpenCL *mCLInterface;
	void Init (RopaOpenCL *rcl) { mCLInterface = rcl; }
	virtual void LockBuffers ();
	virtual void UnlockBuffers ();
};

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------

enum EWorkGroupings { kParticleWorkGrouping, kConstraintWorkGrouping, kVertexWorkGrouping };

class RopaOpenCL
{
public:
	RopaOpenCL () : 
		mContext(0), mCLNumDevices(0), mCPU(0), mGPU(0), mEffect(NULL), mCurrentDevice(0), mProfiling(false)
	{
		memset(mGlobalWorkSize, 0, sizeof(mGlobalWorkSize));
		memset(mLocalWorkSize, 0, sizeof(mLocalWorkSize));

		for (int d = 0; d < kMaxDevices; ++d)
			for (int k = 0; k < kMaxKernels; ++k)
				mIndividualKernelModes[d][k] = 0;
		
#if !CL_GL_INTEROP_WORKING
		mVertexMemory[0] = mVertexMemory[1] = NULL;
#endif
	}
	
	~RopaOpenCL ()
	{
		clReleaseContext(mContext);
	}
	
	void BeginEventStream ()
	{
//		fprintf(stderr, "Clear event stream begin.\n");
		mEventStream.clear();
		mEventNames.clear();
//		fprintf(stderr, "Clear event stream end.\n");
	}
	
	void EndEventStream ()
	{
		if (mProfiling)
		{
			cl_int err;
			clWaitForEvents(1, (cl_event*)&*mEventStream.begin());
			
			printf("------------------------------------------------------------------------\n");
			printf("- Profile info [device=%s, kernel=%d] ----------------------------------\n", mCmdQs[mCurrentDevice].mType, mKernelMode);
			
			size_t numevents = mEventStream.size();
			for (size_t e = 0; e < numevents; ++e)
			{
				cl_ulong timeQueued=0, timeSubmitted=0, timeStarted=0, timeEnded=0;
				cl_event event = (cl_event)mEventStream[e].Get();
				
				err = clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_QUEUED, sizeof(cl_ulong), &timeQueued, NULL);
//				if (err != CL_SUCCESS) 
//					printf("clGetEventProfilingInfo(%s,CL_PROFILING_COMMAND_QUEUED) failed with err = %d\n", mEventNames[e], err);
				
				err = clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_SUBMIT, sizeof(cl_ulong), &timeSubmitted, NULL);
//				if (err != CL_SUCCESS) 
//					printf("clGetEventProfilingInfo(%s,CL_PROFILING_COMMAND_SUBMIT) failed with err = %d\n", mEventNames[e], err);
				
				err = clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &timeStarted, NULL);
//				if (err != CL_SUCCESS) 
//					printf("clGetEventProfilingInfo(%s,CL_PROFILING_COMMAND_START) failed with err = %d\n", mEventNames[e], err);
				
				err = clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &timeEnded, NULL);
//				if (err != CL_SUCCESS) 
//					printf("clGetEventProfilingInfo(%s,CL_PROFILING_COMMAND_END) failed with err = %d\n", mEventNames[e], err);
				
				printf(" %p  %16.3f %16.3f  %s\n", event, ((timeStarted - timeQueued)/1.0e6), (timeEnded - timeStarted)/1.0e6, mEventNames[e]);
			}
			
			printf("------------------------------------------------------------------------\n");
		}
	}
	
	void ProfileEventStream (bool enable)
	{
		if (mProfiling != enable)
		{
			mProfiling = enable;
			for (int d = 0; d < mCLNumDevices; ++d)
			{
				cl_int err = clSetCommandQueueProperty(mCmdQs[d].mCmdQ, CL_QUEUE_PROFILING_ENABLE, enable, NULL);
				if (err != CL_SUCCESS)
					printf("clSetCommandQueueProperty(%d, PROFILING, %s) failed with err = %d\n", d, enable?"true":"false", err);
			}
		}
	}
	
	void AddEvent (const EventID& event, const char* name)
	{
//		fprintf(stderr, "AddEvent(%p, %s) begin\n", event.Get(), name);
		mEventStream.push_back(event);
		mEventNames.push_back(name);
//		fprintf(stderr, "AddEvent end\n");
	}
	cl_int oclGetPlatformID(cl_platform_id* clSelectedPlatformID)
	{
	    char chBuffer[1024];
	    cl_uint num_platforms;
	    cl_platform_id* clPlatformIDs;
	    cl_int ciErrNum;
	    *clSelectedPlatformID = NULL;

	    // Get OpenCL platform count
	    ciErrNum = clGetPlatformIDs (0, NULL, &num_platforms);
	    if (ciErrNum != CL_SUCCESS)
	    {
	        printf(" Error %i in clGetPlatformIDs Call !!!\n\n", ciErrNum);
	        return -1000;
	    }
	    else
	    {
	        if(num_platforms == 0)
	        {
	            printf("No OpenCL platform found!\n\n");
	            return -2000;
	        }
	        else
	        {
	            // if there's a platform or more, make space for ID's
	            if ((clPlatformIDs = (cl_platform_id*)malloc(num_platforms * sizeof(cl_platform_id))) == NULL)
	            {
	                printf("Failed to allocate memory for cl_platform ID's!\n\n");
	                return -3000;
	            }

	            // get platform info for each platform and trap the NVIDIA platform if found
	            ciErrNum = clGetPlatformIDs (num_platforms, clPlatformIDs, NULL);
	            *clSelectedPlatformID = clPlatformIDs[0];
	            free(clPlatformIDs);
	        }
	    }

	    return CL_SUCCESS;
	}

	void Init (void* glContext, const char* opencl_code)
	{
		cl_int err;
		

#if CL_GL_INTEROP_WORKING
		CGLShareGroupObj shareGroup = CGLGetShareGroup((CGLContextObj)glContext);
		mContext = clCreateContextFromCGLShareGroup(shareGroup);
		
		size_t paramsize;
		err = clGetContextInfo(mContext, CL_CONTEXT_DEVICES, sizeof(mCLDevices), mCLDevices, &paramsize);
		if (err != CL_SUCCESS) { printf("clGetContextInfo error = %d\n", err); exit(1); }
		mCLNumDevices = paramsize/sizeof(cl_device_id);
#else
	#if USE_ONLY_CPU_DEVICES
		err = clGetDeviceIDs(NULL, CL_DEVICE_TYPE_CPU, kMaxDevices, mCLDevices, &mCLNumDevices);
	#elif USE_ONLY_GPU_DEVICES
		err = clGetDeviceIDs(NULL, CL_DEVICE_TYPE_GPU, kMaxDevices, mCLDevices, &mCLNumDevices);
	#else
		cl_platform_id clSelectedPlatformID = NULL;
		cl_int ciErrNum =  oclGetPlatformID (&clSelectedPlatformID);
	    if(ciErrNum !=CL_SUCCESS)
	    { printf("clGetPlatformID error = %d\n",ciErrNum); exit(1);}
	    err = clGetDeviceIDs(clSelectedPlatformID, CL_DEVICE_TYPE_ALL, kMaxDevices, mCLDevices, &mCLNumDevices);

	#endif
		if (err != CL_SUCCESS) { printf("clGetDeviceIDs error = %d\n", err); exit(1); };

		mContext = clCreateContext(0, mCLNumDevices, mCLDevices, clNotifyError, NULL, &err);
		if (err != CL_SUCCESS) { printf("clCreateContext error = %d\n", err); exit(1); };
		if (mContext == (cl_context)0) { printf("clCreateContext returned zero\n"); exit(1); };
#endif

		printf("Found %d OpenCL devices.\n", mCLNumDevices);

		mCPU = mGPU = -1;
		for (int i = 0; i < mCLNumDevices; ++i)
		{
			cl_device_type devtype;
			err = clGetDeviceInfo(mCLDevices[i], CL_DEVICE_TYPE, sizeof(cl_device_type), &devtype, NULL);
			if (err == CL_SUCCESS)
			{
				if (devtype == CL_DEVICE_TYPE_CPU)
				{
					printf("Found device %d is CPU\n", i);
					mCPU = i;
				}
				else if (devtype == CL_DEVICE_TYPE_GPU)
				{
					printf("Found device %d is GPU\n", i);
					mGPU = i;
				}
				else
					printf("Found device %d is type %d... this lame demo doesn't know what to do with it. CL_DEVICE_TYPE_GPU = %d \n", i, (int)devtype,CL_DEVICE_TYPE_GPU);
			}
			
			err = clGetDeviceInfo(mCLDevices[i], CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(mCLDeviceComputeUnits[i]), &mCLDeviceComputeUnits[i], NULL);
			if (err == CL_SUCCESS)
				printf("Device %d claims to have %d compute units\n", i, mCLDeviceComputeUnits[i]);
		}
		
		if (mCPU == -1)
		{
			printf("Did not find CPU device!\n");
			mCPU = 0;
		}
		if (mGPU == -1)
		{
			printf("Did not find GPU device!\n");
			mGPU = 0;
		}
		
		mCmdQs[mCPU].Create(mContext, mCLDevices[mCPU], "cpu");
		mCmdQs[mGPU].Create(mContext, mCLDevices[mGPU], "GPU");

		mProgram.Load(mContext, mCLNumDevices, mCLDevices, opencl_code, mCPU, mGPU, mAPU);
		mIntegrator.Init(this, ROPA::kNumSolvers+0);
		if(this->distanceSolverOpts.useTM)
			mDistanceSolver.Init_tm(this, ROPA::kParticleDistanceSolver);
		else if(this->distanceSolverOpts.useAtomic)
         mDistanceSolver.Init_atomic(this, ROPA::kParticleDistanceSolver);
		else
			mDistanceSolver.Init(this, ROPA::kParticleDistanceSolver);
		mDriverSolver.Init(this, ROPA::kDriverSolver);
		mSphereSolver.Init(this, ROPA::kSphereSolver);
		mTubeSolver.Init(this, ROPA::kTubeSolver);
		mWriteBack.Init(this, ROPA::kNumSolvers+1);
		mLockBuffers.Init(this);			
	}

	CLKernel* GetKernel (int kernelindex)
	{
		switch (kernelindex)
		{
			case ROPA::kParticleDistanceSolver:
				return &mDistanceSolver.mKernel;
			case ROPA::kDriverSolver:
				return &mDriverSolver.mKernel;
			case ROPA::kSphereSolver:
				return &mSphereSolver.mKernel;
			case ROPA::kTubeSolver:
				return &mTubeSolver.mKernel;
			case ROPA::kNumSolvers+0:
				return &mIntegrator.mKernel; 
			case ROPA::kNumSolvers+1:
				return &mWriteBack.mKernel;
			case ROPA::kNullSolver:
			case ROPA::kCubeSolver:
			case ROPA::kPlaneSolver:
			default:
				return NULL;
		}
		return NULL;
	}
	
	void SetWorkSize (int count, int simd_width_divisor, int kernelindex, int kernelmode, int deviceindex)
	{
		bool isTaskMode = ((kernelmode == 0) || (kernelmode == 3));
		size_t local = 1, global = 1;
		
		cl_int err;
		size_t maxWorkGroupSize = 1;
		CLKernel* kernel = this->GetKernel(kernelindex);
		if (kernel != NULL)
		{
			err = clGetKernelWorkGroupInfo(kernel->Get(kernelmode), mCLDevices[deviceindex], CL_KERNEL_WORK_GROUP_SIZE, sizeof(maxWorkGroupSize), &maxWorkGroupSize, NULL);
			if (err != CL_SUCCESS)
				printf("clGetKernelWorkGroupInfo failed with err=%d\n", err);
			
			//printf("Max work group size (kernelindex=%d, kernelmode=%d, device=%d) = %ld\n", kernelindex, kernelmode, deviceindex, maxWorkGroupSize);
		}
		else
			isTaskMode = true;
		
		if (!isTaskMode)
		{
			size_t numWorkGroups = (count + maxWorkGroupSize * simd_width_divisor - 1) / (maxWorkGroupSize * simd_width_divisor);
				
			local = (count + (numWorkGroups * simd_width_divisor - 1)) / (numWorkGroups * simd_width_divisor);
			global = numWorkGroups * local;
			
			if (kernelindex == ROPA::kParticleDistanceSolver)
			{	// this solver can only work with a single workgroup
				if (kernelmode >= 2)
				{
					local = 1;
					global = count;
				}
				else if (maxWorkGroupSize < 8)
				{
					local = 1;
					global = count/8;
				}
				else
				{
					local = 8;
					global = count;
				}
			}
			else
			{	// where possible we want multiple overlapping work groups
				if (((local/5)*5 == local) && (kernelmode == 2))
					local /= 5;
				else if ((local & 1) == 0)
					local /= 2;
			}
			
			if (deviceindex == mCPU)
			{
				if (global > mCLDeviceComputeUnits[deviceindex]*kComputeUnitMultiplier)
				{
					global = mCLDeviceComputeUnits[deviceindex]*kComputeUnitMultiplier;
					local = (maxWorkGroupSize < mCLDeviceComputeUnits[deviceindex]) ? 1 : mCLDeviceComputeUnits[deviceindex];
				}
			}
		}
		


		if(kernelindex != ROPA::kParticleDistanceSolver) {
        		 mGlobalWorkSize[kernelindex][kernelmode][deviceindex] = 256*30;
         		 mLocalWorkSize[kernelindex][kernelmode][deviceindex] = 256;
		} else {
		  	 mGlobalWorkSize[kernelindex][kernelmode][deviceindex] = global;
         		mLocalWorkSize[kernelindex][kernelmode][deviceindex] = local;
		}
	}

	void ComputeWorkSizes (int deviceindex, int kernelmode, int numVerts, int numParticles, int numVolumes, int numConstraints, int constraintMultiple, int *numOctets)
	{
		this->SetWorkSize(numParticles, (kernelmode>=2)?4:1, ROPA::kNumSolvers+0, kernelmode, deviceindex);
		this->SetWorkSize(numVerts, (kernelmode>=2)?4:1, ROPA::kNumSolvers+1, kernelmode, deviceindex);
		
		// The tricky part here is that we want each octet processed in order.  
		// This means that 8 items process concurrently, and successive sets of 8 process after a barrier.
		// Only 1 work group is created and each work item iterates across all octets, processing its element of each octet
		for (int c = 0; c < numConstraints; ++c)
		{
			if (mEffect->GetConstraintSetSolver(c) == ROPA::kDriverSolver)
			{	// driver constraint is the exception -- it can be done by particle, but the kernel's SIMD width is 8 in this case
				this->SetWorkSize(numParticles, (kernelmode>=2)?8:1, mEffect->GetConstraintSetSolver(c), kernelmode, deviceindex);
			}
			else
			{
				if (kernelmode>=2) constraintMultiple /= 8;		// adjust for the kernel's SIMD width
				this->SetWorkSize(constraintMultiple, 1, mEffect->GetConstraintSetSolver(c), kernelmode, deviceindex);
			}
		}
	}
	
	void SetupData (ROPA::Effect* effect, Vector4 *vertices, GLuint *vertexbuffers, int octetGroupSize)
	{
		int numVerts = effect->GetNbVertices();
		int numParticles = effect->GetNbParticles();
		printf("Setupdata - num of particles = %d\n", numParticles);
		int numVolumes = effect->GetNbVolumes();
		int numConstraintSets = effect->GetNbConstraintSets();
		int numOctets[16];
		
		mEffect = effect;
		mOctetGroupSize = octetGroupSize;
		mIterations = effect->GetConstraintTuningParameters().mNbIterations;

//		printf("SetupData:  iState=%p, ctp=%p, particles=%p, particlesDebug=%p, numparticles=%d, octets=%p, numoctets=%d, drivers=%p\n",
//			   &effect->GetIntegratorState(), &effect->GetConstraintTuningParameters(), effect->GetParticles(), effect->GetParticlesDebug(), numParticles, 
//			   effect->GetDriverConstraints()->GetConstraintsArray(), effect->GetDriverConstraints()->GetNbConstraintOctets(), effect->GetDrivers());
//		printf("sizeof octet=%ld, driver=%ld, particle=%ld, particledebug=%ld\n", sizeof(ROPA::ConstraintOctet), sizeof(ROPA::PosNormPair), sizeof(ROPA::Particle), sizeof(ROPA::ParticleDebug));
		
		mCLIntegratorState.Create(mContext, &effect->GetIntegratorState(), sizeof(ROPA::IntegratorState), CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR);
		mCLConstraintTuningParameters.Create(mContext, (void*)&effect->GetConstraintTuningParameters(), sizeof(ROPA::ConstraintTuningParameters), CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);
		
		mCLVertices.Create(mContext, vertices, sizeof(Vector4)*numVerts, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);
		mCLParticles.Create(mContext, effect->GetParticles(), sizeof(ROPA::Particle)*numParticles, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);
		mCLParticlesDebug.Create(mContext, effect->GetParticlesDebug(), sizeof(ROPA::ParticleDebug)*numParticles, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR);
		mCLDrivers.Create(mContext, effect->GetDrivers(), sizeof(ROPA::PosNormPair)*numParticles, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);
		mCLVolumes.Create(mContext, effect->GetVolumesTransform(0), sizeof(ROPA::VolumeTransform)*numVolumes, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);
		mCLVertexMapping.Create(mContext, (void*)effect->GetVertexMappingArray(), sizeof(int16_t)*numParticles, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);
		mCLReverseMapping.Create(mContext, (void*)effect->GetReverseVertexMappingArray(), sizeof(uint16_t)*numVerts, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);
		mCLMappedVertices.Create(mContext, (void*)effect->GetMappedVertexFlags(), (sizeof(uint32_t)*numParticles+31)/32, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);

		mCLAtomicLocks.Create(mContext, effect->GetAtomicLocks(), sizeof(int)*numParticles, CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);

		for (int i = 0; i < numConstraintSets; ++i)
		{
			ROPA::ConstraintsSet* cs = effect->GetConstraintsSet(i);
			numOctets[i] = cs->GetNbConstraintOctets();
			mCLConstraintOctets[i].Create(mContext, cs->GetConstraintsArray(), sizeof(ROPA::ConstraintOctet)*numOctets[i], CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR/*CL_MEM_USE_HOST_PTR*/);
			printf("SetupData: constraintset=%d, numOctets=%d\n", i, numOctets[i]);
		}
		
#if CL_GL_INTEROP_WORKING
		mOutputBuffers[0].Create(mContext, vertexbuffers[0], CL_MEM_WRITE_ONLY);
		mOutputBuffers[1].Create(mContext, vertexbuffers[1], CL_MEM_WRITE_ONLY);
#else
		mVertexMemory[0] = new char[16*numVerts];
		mVertexMemory[1] = new char[16*numVerts];
		mOutputBuffers[0].Create(mContext, mVertexMemory[0], 16*numVerts, CL_MEM_WRITE_ONLY|CL_MEM_COPY_HOST_PTR);
		mOutputBuffers[1].Create(mContext, mVertexMemory[1], 16*numVerts, CL_MEM_WRITE_ONLY|CL_MEM_COPY_HOST_PTR);

		/*
		printf("==================SOURCE=============================================\n");
		Print_Memory((unsigned char*)mVertexMemory[0],16*numVerts);
		void * output_buffer = (void *) malloc (16*numVerts);
		cl_command_queue q = mCmdQs[mCurrentDevice].mCmdQ;
		cl_int err = clEnqueueReadBuffer(q, mOutputBuffers[0].mBuffer, true, 0, 16*numVerts, output_buffer, 0, NULL,NULL);
		printf("=================DEST==================================================\n");
		Print_Memory((unsigned char *)output_buffer,16*numVerts);
		*/


#endif
		
		for (int km=0; km<kNumKernelModes; ++km)
			for (int dev=0; dev<mCLNumDevices;++dev)
				this->ComputeWorkSizes(dev, km, numVerts, numParticles, numVolumes, numConstraintSets, 8*octetGroupSize, numOctets);
		
		printf("------------------------------------------------------------\n");
		printf("numVerts = %d, numParticles = %d, numVolumes = %d, numConstraintSets = %d octetGroupSize = %d", numVerts, numParticles, numVolumes, numConstraintSets, 8*octetGroupSize);

		// kernels we care to print...
		const int numKernelsToPrint = 4;
		int kernelindextable[numKernelsToPrint] = { ROPA::kNumSolvers+0, ROPA::kNumSolvers+1, ROPA::kParticleDistanceSolver, ROPA::kDriverSolver };
		const char* kernelindexnames[numKernelsToPrint] = { "Integrator", "WriteBack", "Distance", "Driver" };
		
		for (int c=0; c<numConstraintSets;++c)
			printf(", numOctets[%d] = %d", c, numOctets[c]);
		printf("\n");
		for (int km=0; km<kNumKernelModes;++km)
		{
			printf("Kernel mode = %s\n------------------------------------------------------------\n", kKernelModeNames[km]);
			for (int dev=0; dev<kMaxDevices;++dev)
			{
				printf("Device %s:\n", mCmdQs[dev].mType);
				for (int ki=0; ki<numKernelsToPrint;++ki)
					printf("kernel %16s:		global=%8ld	local=%8ld\n", kernelindexnames[ki], 
						   mGlobalWorkSize[kernelindextable[ki]][km][dev], mLocalWorkSize[kernelindextable[ki]][km][dev]);
				printf("------------------------------------------------------------\n");
			}
		}
	}
	
	void ReleaseData ()
	{
		mCLVertices.Release();
		mCLParticles.Release();
		mCLParticlesDebug.Release();
		mCLDrivers.Release();
		mCLVolumes.Release();
		mCLVertexMapping.Release();
		mCLReverseMapping.Release();
		mCLMappedVertices.Release();

		mCLAtomicLocks.Release();

		if (mEffect != NULL)
			for (int i = 0; i < mEffect->GetNbConstraintSets(); ++i)
				mCLConstraintOctets[i].Release();
		
		mOutputBuffers[0].Release();
		mOutputBuffers[1].Release();
		
#if !CL_GL_INTEROP_WORKING
		delete [] mVertexMemory[0];
		delete [] mVertexMemory[1];
#endif
	}
	
	EventID WriteToBuffer (EventID event, const CLBuffer& clBuffer, const void* data, size_t offset, size_t bytes)
	{
		cl_event wevent = (cl_event)event.Get();
		cl_int numevents = event.IsValid()?1:0;
		cl_event *events = event.IsValid()?(cl_event*)event.GetPtrTo():NULL;
		cl_command_queue q = mCmdQs[mCurrentDevice].mCmdQ;
		cl_int err = clEnqueueWriteBuffer(q, clBuffer.mBuffer, false, offset, bytes, data, numevents, events, &wevent);
		if (err != CL_SUCCESS)
		{
			printf("WriteToBuffer failed with err=%d\n", err);
			return event;
		}
		else
		{
			EventID result(wevent, "CreateForWriteToBuffer");
			this->AddEvent(result, "WriteToBuffer");
			return result;
		}
	}
	
	EventID ReadFromBuffer (EventID event, const CLBuffer& clBuffer, void* data, size_t offset, size_t bytes)
	{
		cl_event revent = (cl_event)event.Get();
		cl_int numevents = event.IsValid()?1:0;
		cl_event *events = event.IsValid()?(cl_event*)event.GetPtrTo():NULL;
		cl_command_queue q = mCmdQs[mCurrentDevice].mCmdQ;
	     
		/*	_cl_mem * Integrator;
		_cl_mem * Particles;
 		_cl_mem * ParticlesDebug;
 		_cl_mem * out1 ;
 		_cl_mem * out2 ;

 		size_t Integrator1 = 0x10000000;
 		size_t Particles1 = 0x100040c0;
 		size_t ParticlesDebug1 = 0x1000a9c0;
 		size_t out11 = 0x1001b580;
 		size_t out21 = 0x1001f540;

 		Integrator = (cl_mem) Integrator1;
		
 		wrapper_print_cl_mem(Integrator,q);
 		wrapper_print_cl_mem(Particles,q);
 		wrapper_print_cl_mem(ParticlesDebug,q);
 		wrapper_print_cl_mem(out1,q);
 		wrapper_print_cl_mem(out2,q);*/

		cl_int err = clEnqueueReadBuffer(q, clBuffer.mBuffer, true, offset, bytes, data, numevents, events, &revent);
		if (err != CL_SUCCESS)
		{
			printf("ReadFromBuffer failed with err=%d\n", err);
			return event;
		}
		else
		{
			EventID result(revent, "CreateForReadFromBuffer");
			this->AddEvent(result, "ReadFromBuffer");
			//printf("<====================DEST=====>");
			//Print_Memory((unsigned char*)(clBuffer.mBuffer->host_ptr()),bytes);
			return result;
		}
	}
						   
	EventID AcquireOutputBuffers (const EventID& event)
	{
#if CL_GL_INTEROP_WORKING
		cl_event aevent = (cl_event)event.Get();
		cl_int err = CL_SUCCESS;
		cl_int numevents = event.IsValid()?1:0;
		cl_event *events = event.IsValid()?(cl_event*)&event:NULL;
		cl_command_queue q = mCmdQs[mCurrentDevice].mCmdQ;
		err = clEnqueueAcquireGLObjects(q, 1, &mOutputBuffers[mCurrentOutput].mCLBuffer, numevents, events, &aevent);
		if (err != CL_SUCCESS)
		{
			printf("AcquireOutputBuffers failed with err=%d\n", err);
			return event;
		}
		else
		{
			EventID result(aevent, "CreateForAcquireOutputBuffers");
			this->AddEvent(result, "AcquireOutputBuffer");
			return result;
		}
#else
		return event;
#endif
	}
	
	EventID ReleaseOutputBuffers (const EventID& event)
	{
#if CL_GL_INTEROP_WORKING
		cl_event revent = (cl_event)event.Get();
		cl_int err = CL_SUCCESS;
		cl_int numevents = event.IsValid()?1:0;
		cl_event *events = event.IsValid()?(cl_event*)&event:NULL;
		cl_command_queue q = mCmdQs[mCurrentDevice].mCmdQ;
		err = clEnqueueReleaseGLObjects(q, 1, &mOutputBuffers[mCurrentOutput].mCLBuffer, numevents, events, &revent);
		if (err != CL_SUCCESS)
		{
			printf("ReleaseOutputBuffers failed with err=%d\n", err);
			return event;
		}
		else
		{
			EventID result(revent, "CreateForReleaseOutputBuffers");
			this->AddEvent(result, "ReleaseOutputBuffer");
			return result;
		}
#else
		return event;
#endif
	}
	
	void SetKernelArg (const CLKernel& kernel, unsigned int index, size_t arg_size, const void* arg_value)
	{
//		printf("SetKernelArg(index=%d, data=%p, bytes=%ld)\n", index, arg_value, arg_size);
		
		cl_kernel k = kernel.Get(mIndividualKernelModes[mCurrentDevice][kernel.mIndex]);
		cl_int err = clSetKernelArg(k, index, arg_size, arg_value);
		if (err != CL_SUCCESS)
			printf("SetKernelArg(index=%d,size=%ld) failed with err=%d\n", index, arg_size, err);
	}
	
	void SetKernelBufferArg (CLKernel& kernel, unsigned int index, const CLBuffer& buffer)
	{
		cl_kernel k = kernel.Get(mIndividualKernelModes[mCurrentDevice][kernel.mIndex]);
		buffer.SetAsArg(k, index);
	}
	
	void SetKernelBufferArg (const CLKernel& kernel, unsigned int index, const CLGLBuffer& buffer)
	{
		cl_kernel k = kernel.Get(mIndividualKernelModes[mCurrentDevice][kernel.mIndex]);
		buffer.SetAsArg(k, index);
	}
	
	EventID InvokeKernel (CLKernel& kernel, const EventID& event, bool doIterations)
	{
		unsigned int work_dim = 1;		// all ROPA kernels are 1 dimensional
		size_t* global_work_size;
		size_t* local_work_size;
		global_work_size = &mGlobalWorkSize[kernel.mIndex][mIndividualKernelModes[mCurrentDevice][kernel.mIndex]][mCurrentDevice];
		local_work_size = &mLocalWorkSize[kernel.mIndex][mIndividualKernelModes[mCurrentDevice][kernel.mIndex]][mCurrentDevice];

		unsigned int numEvents = 0;
		cl_event events[1], *eventlist = events;
		events[0] = (cl_event)event.Get();
		if (event.IsValid())
			numEvents = 1;
		else
			eventlist = NULL;

		cl_event kevent = (cl_event)event.Get();
		cl_kernel k = kernel.Get(mIndividualKernelModes[mCurrentDevice][kernel.mIndex]);
		cl_command_queue q = mCmdQs[mCurrentDevice].mCmdQ;

		// all ROPA kernels have same first 5 parameters:  iState, ctp, particles, particles debug, numparticles
		mCLIntegratorState.SetAsArg(k, 0);
		mCLConstraintTuningParameters.SetAsArg(k, 1);
		mCLParticles.SetAsArg(k, 2);
		mCLParticlesDebug.SetAsArg(k, 3);

		if(kernel.mIndex == ROPA::kParticleDistanceSolver && this->distanceSolverOpts.useAtomic) {
		   mCLAtomicLocks.SetAsArg(k,7);
		}

		cl_int err;
		cl_int numparticles = mEffect->GetNbParticles();
		this->SetKernelArg(kernel, 4, sizeof(numparticles), &numparticles);

		//printf("kernel(%s, numparticles=%d, work_dim=%d, global_work_size=%ld, local_work_size=%ld, num_events=%d)\n", kernel.GetName(mKernelMode), numparticles, work_dim, *global_work_size, *local_work_size, numEvents);
		int numIterations = doIterations ? mIterations : 1;
		for (int iterations = 0;  iterations < numIterations; ++iterations)
		{
#if INDIVIDUAL_KERNEL_TIMING
			uint64_t start = mach_absolute_time();
#endif


			// For TM, set global and local worksizes
			if(kernel.mIndex == ROPA::kParticleDistanceSolver && this->distanceSolverOpts.useTM) {
				const ROPA::ConstraintsSet* cs = this->mEffect->GetConstraintsSet(0);
				cl_int clNumOctets = cs->GetNbConstraintOctets();
            *local_work_size = local_ws;
				*global_work_size = (clNumOctets*8/(*local_work_size) + 1)*(*local_work_size);
				printf("Set TM global and local worksizes.\n");
			}

			// For Atomics, set global and local worksizes
			if(kernel.mIndex == ROPA::kParticleDistanceSolver && this->distanceSolverOpts.useAtomic) {
            const ROPA::ConstraintsSet* cs = this->mEffect->GetConstraintsSet(0);
            cl_int clNumOctets = cs->GetNbConstraintOctets();
            *local_work_size = local_ws;
            *global_work_size = (clNumOctets*8/(*local_work_size) + 1)*(*local_work_size);
            printf("Set Atomic global and local worksizes.\n");
         }

			printf("Invoking kernel: %s (index=%d)  with global work size: %d and local work size %d \n",kernel.mScalarName,kernel.mIndex,*global_work_size,*local_work_size);

			// debug variables
			int kernelToDebug = -1;// 7 1 2 8

			// Dump values

			if(kernel.mIndex == kernelToDebug) {
				FILE  *myfile;
				myfile = fopen("Dump_values.txt","w");

				ROPA::IntegratorState& IntegratorState_dump = *((ROPA::IntegratorState*)mCLIntegratorState.mSource);
				fprintf(myfile, "IntegratorState: \n");
				fprintf(myfile, "  mLastDT=%14.10f \n", IntegratorState_dump.mLastDT);
				fprintf(myfile, "  mLastFullDT=%14.10f \n", IntegratorState_dump.mLastFullDT);
				fprintf(myfile, "  mSpeed=%f \n", IntegratorState_dump.mSpeed);
				fprintf(myfile, "  mReset=%d \n", IntegratorState_dump.mReset);
				fprintf(myfile, "  mLastDTsIndex=%d \n", IntegratorState_dump.mLastDTsIndex);
				fprintf(myfile, "  mNbIntegrationSteps=%d \n", IntegratorState_dump.mNbIntegrationSteps);
				fprintf(myfile, "  mLastDTs= [");
				for(int i=0; i<30; i++) fprintf(myfile, "%14.10f, ", IntegratorState_dump.mLastDTs[i]);
				fprintf(myfile, "] \n");
				fprintf(myfile, "\n");

				ROPA::ConstraintTuningParameters& ConstraintTuningParameters_dump = *((ROPA::ConstraintTuningParameters*)mCLConstraintTuningParameters.mSource);
				fprintf(myfile, "ConstraintTuningParameters: \n");
				fprintf(myfile, "  mMaxDTRatio=%24.20f \n", ConstraintTuningParameters_dump.mMaxDTRatio);
				fprintf(myfile, "  mMinDTIntegration=%24.20f \n", ConstraintTuningParameters_dump.mMinDTIntegration);
				fprintf(myfile, "  mFixedDT=%24.20f \n", ConstraintTuningParameters_dump.mFixedDT);
				fprintf(myfile, "  mVerticalSpeedDampening=%24.20f \n", ConstraintTuningParameters_dump.mVerticalSpeedDampening);
				fprintf(myfile, "  mCompression=%24.20f \n", ConstraintTuningParameters_dump.mCompression);
				fprintf(myfile, "  mVertDistanceScale=%24.20f \n", ConstraintTuningParameters_dump.mVertDistanceScale);
				fprintf(myfile, "  mNormDistanceScale=%24.20f \n", ConstraintTuningParameters_dump.mNormDistanceScale);
				fprintf(myfile, "  mNbIterations=%d \n", ConstraintTuningParameters_dump.mNbIterations);
				fprintf(myfile, "  mUseConstraintLengthOnNormals=%d \n", ConstraintTuningParameters_dump.mUseConstraintLengthOnNormals);
				fprintf(myfile, "\n");

				fprintf(myfile, "numparticles=%d\n", numparticles);
				fprintf(myfile, "\n");

				fclose(myfile);


				myfile = fopen("Dump_particles.txt","w");
				ROPA::Particle* Particles_dump = (ROPA::Particle*)mCLParticles.mSource;
				for(unsigned int i=0; i<numparticles; i++){
					fprintf(myfile, "%.14f %.14f %.14f %.14f ", Particles_dump[i].mPos.v[0], Particles_dump[i].mPos.v[1], Particles_dump[i].mPos.v[2], Particles_dump[i].mPos.v[3]);
					fprintf(myfile, "%.14f %.14f %.14f %.14f ", Particles_dump[i].mPrevPos.v[0], Particles_dump[i].mPrevPos.v[1], Particles_dump[i].mPrevPos.v[2], Particles_dump[i].mPrevPos.v[3]);
					fprintf(myfile, "\n");
				}
				fclose(myfile);

				myfile = fopen("Dump_particlesdebug.txt","w");
				ROPA::ParticleDebug* ParticlesDebug_dump = (ROPA::ParticleDebug*)mCLParticlesDebug.mSource;
				for(unsigned int i=0; i<numparticles; i++){
					fprintf(myfile, "%u %.14f \n", ParticlesDebug_dump[i].mColor, ParticlesDebug_dump[i].mLength);
				}
				fclose(myfile);
			}



			// Print buffers before kernel
			if(kernel.mIndex == kernelToDebug) {
				printf("\nPrint buffers before kernel \n");

				printf("ConstraintTuningParameters (before) \n:");
				Print_Memory2("hw_dis_b_constun.txt", "w", (unsigned char*) mCLConstraintTuningParameters.mSource, mCLConstraintTuningParameters.mBytes);
				printf("Octets (before) \n");
				Print_Memory2("hw_dis_b_octet.txt", "w", (unsigned char*) mCLConstraintOctets[0].mSource, mCLConstraintOctets[0].mBytes);
				printf("Integrator state (before) \n");
				Print_Memory2("hw_dis_b_int.txt", "w", (unsigned char*) mCLIntegratorState.mSource, mCLIntegratorState.mBytes);
				printf("Particles (before) \n");
				Print_Memory2("hw_dis_b_part.txt", "w", (unsigned char*) mCLParticles.mSource, mCLParticles.mBytes);
				printf("Particles Debug (before) \n");
				Print_Memory2("hw_dis_b_partd.txt", "w", (unsigned char*) mCLParticlesDebug.mSource, mCLParticlesDebug.mBytes);
			}

			// Atomics debug (before)
			if(0 && kernel.mIndex == ROPA::kParticleDistanceSolver && this->distanceSolverOpts.useAtomic) {
			   printf("Atomic locks (before): \n");
			   Print_Memory_ints("atomic_locks_b.txt", "w", (int*) mCLAtomicLocks.mSource, numparticles);
			}


			err = clEnqueueNDRangeKernel(q, k, work_dim, NULL, global_work_size, local_work_size, numEvents, eventlist, &kevent);


			// Atomics debug (after)
         if(0 && kernel.mIndex == ROPA::kParticleDistanceSolver && this->distanceSolverOpts.useAtomic) {
            printf("Atomic locks (after): \n");
            unsigned char* data_locks = (unsigned char*) malloc(mCLAtomicLocks.mBytes);
            clEnqueueReadBuffer(q, mCLAtomicLocks.mBuffer, true, 0, mCLAtomicLocks.mBytes, data_locks, 0, NULL, NULL);
            Print_Memory_ints("atomic_locks_a.txt", "w", (int*) data_locks, numparticles);
         }


			// Print buffers after kernel
			if(kernel.mIndex == kernelToDebug) {
				printf("\nPrint buffers after kernel \n");
				printf("\nParticle buffer size = %d\n", mCLParticles.mBytes);
				unsigned char* data_int = (unsigned char*) malloc(mCLIntegratorState.mBytes);
				unsigned char* data_part = (unsigned char*) malloc(mCLParticles.mBytes);
				unsigned char* data_partd = (unsigned char*) malloc(mCLParticlesDebug.mBytes);
				clEnqueueReadBuffer(q, mCLIntegratorState.mBuffer, true, 0, mCLIntegratorState.mBytes, data_int, 0, NULL, NULL);
				clEnqueueReadBuffer(q, mCLParticles.mBuffer, true, 0, mCLParticles.mBytes, data_part, 0, NULL, NULL);
				clEnqueueReadBuffer(q, mCLParticlesDebug.mBuffer, true, 0, mCLParticlesDebug.mBytes, data_partd, 0, NULL, NULL);
				printf("Integrator state (after) \n");
				Print_Memory2("hw_dis_a_int.txt", "w", data_int, mCLIntegratorState.mBytes);
				printf("Particles (after) \n");
				Print_Memory2("hw_dis_a_part.txt", "w", data_part, mCLParticles.mBytes);
				printf("Particles Debug (after) \n");
				Print_Memory2("hw_dis_a_partd.txt", "w", data_part, mCLParticlesDebug.mBytes);
				free(data_int);
				free(data_part);
				free(data_partd);
				exit(0);
			}


#if INDIVIDUAL_KERNEL_TIMING
			clWaitForEvents(1, &kevent);
			uint64_t stop = mach_absolute_time();
			kernel.AccumulateTime(stop - start);
#endif

			eventlist = events;
			events[0] = kevent;
			numEvents = 1;
		}
		

		if (err != CL_SUCCESS)
		{
			printf("InvokeKernel on %s failed with err=%d [g=%ld, l=%ld]\n", kernel.GetName(mIndividualKernelModes[mCurrentDevice][kernel.mIndex]), err, *global_work_size, *local_work_size);
			return event;
		}
		else
		{
			EventID result(kevent, "CreateForInvokeKernel");
			this->AddEvent(result, kernel.GetName(mIndividualKernelModes[mCurrentDevice][kernel.mIndex]));
			return result;
		}
	}
	
public:
	DistanceSolverOptions	distanceSolverOpts;
	int               local_ws;

	cl_context				mContext;
	cl_uint					mCLNumDevices;
	cl_device_id			mCLDevices[kMaxDevices];
	cl_uint					mCLDeviceComputeUnits[kMaxDevices];
	CLCmdQueue				mCmdQs[kMaxDevices];
	int						mCurrentDevice;
	
	int						mCPU, mGPU, mAPU;
	int						mKernelMode;
	int						mIndividualKernelModes[kMaxDevices][kMaxKernels];
	
	CLProgram				mProgram;
	
	bool					mProfiling;
	std::vector<EventID>	mEventStream;
	std::vector<const char*> mEventNames;

	ROPA::Effect*			mEffect;
	cl_int					mOctetGroupSize;
	int						mIterations;
	
	// indexed by [kernelindex][kernelmode][deviceindex]
	size_t					mGlobalWorkSize[kMaxKernels][kNumKernelModes][kMaxDevices];
	size_t					mLocalWorkSize[kMaxKernels][kNumKernelModes][kMaxDevices];
	
	opencl_RopaIntegrator	mIntegrator;
	opencl_DistanceSolver	mDistanceSolver;
	opencl_DriverSolver		mDriverSolver;
	opencl_DriverSolver		mSphereSolver;
	opencl_DriverSolver		mTubeSolver;
	opencl_RopaWriteBack	mWriteBack;
	opencl_LockBuffers		mLockBuffers;
	
	CLBuffer				mCLIntegratorState;
	CLBuffer				mCLConstraintTuningParameters;
	CLBuffer				mCLVertices;
	CLBuffer				mCLParticles;
	CLBuffer				mCLParticlesDebug;
	CLBuffer				mCLDrivers;
	CLBuffer				mCLVolumes;
	CLBuffer				mCLConstraintOctets[16];
	CLBuffer				mCLVertexMapping;
	CLBuffer				mCLReverseMapping;
	CLBuffer				mCLMappedVertices;
	
	CLBuffer          mCLAtomicLocks;

#if CL_GL_INTEROP_WORKING
	int						mCurrentOutput;
	CLGLBuffer				mOutputBuffers[2];
#else
	int						mCurrentOutput;
	CLBuffer				mOutputBuffers[2];
	char*					mVertexMemory[2];
#endif
};

//------------------------------------------------------------------------------------------------------------------------------------------------------------------------

void opencl_RopaIntegrator::Init (RopaOpenCL *rcl, int kernelindex)
{
	mCLInterface = rcl;
	mKernel.Create(rcl->mProgram.mProgram, kernelindex, "task_integrator", "scalar_integrator", "vector_integrator");
}
	
EventID opencl_RopaIntegrator::operator() (EventID event, float dt, const Matrix44& deltaPos, const Vector4& gravity, const ROPA::ConstraintTuningParameters& ctp, ROPA::IntegratorState& istate)
{
	// InvokeKernel sets the first 5 parameters
	mCLInterface->SetKernelBufferArg(mKernel, 5, mCLInterface->mCLVertexMapping);
	mCLInterface->SetKernelBufferArg(mKernel, 6, mCLInterface->mCLMappedVertices);
	mCLInterface->SetKernelArg(mKernel, 7, sizeof(deltaPos), &deltaPos);
	mCLInterface->SetKernelArg(mKernel, 8, sizeof(gravity), &gravity);
	mCLInterface->SetKernelArg(mKernel, 9, sizeof(dt), &dt);
	return mCLInterface->InvokeKernel(mKernel, event, true);
}



void opencl_RopaWriteBack::Init (RopaOpenCL *rcl, int kernelindex) 
{
	mCLInterface = rcl; 
	mKernel.Create(rcl->mProgram.mProgram, kernelindex, "task_writeback", "scalar_writeback", "vector_writeback");
}

EventID opencl_RopaWriteBack::operator() (EventID event, const Matrix44& mat)
{
	// Enqueue an asynchronous write to the transform buffer, from a local copy we know is persistent (otherwise would need to do blocking write)
	
	// Need output buffers before the write back can happen
	event = mCLInterface->AcquireOutputBuffers(event);
	
	// InvokeKernel sets the first 5 parameters
	mCLInterface->SetKernelArg(mKernel, 5, sizeof(mat), &mat);
	mCLInterface->SetKernelBufferArg(mKernel, 6, mCLInterface->mOutputBuffers[mCLInterface->mCurrentOutput]);
	mCLInterface->SetKernelBufferArg(mKernel, 7, mCLInterface->mCLReverseMapping);
	cl_int numverts = mCLInterface->mEffect->GetNbVertices();
	mCLInterface->SetKernelArg(mKernel, 8, sizeof(numverts), &numverts);

	event = mCLInterface->InvokeKernel(mKernel, event, false);
	
	// Release output buffers after write back kernel executes so GL can use them
	event = mCLInterface->ReleaseOutputBuffers(event);
	
	return event;
}



EventID opencl_GenericSolver::operator() (EventID event, int constraintIndex, int numOctets, int numIters, short refObjIdx)
{
	// InvokeKernel sets the first 5 parameters, all constraints get numoctets, octets as next 2
	mCLInterface->SetKernelBufferArg(mKernel, 5, mCLInterface->mCLConstraintOctets[constraintIndex]);
	const ROPA::ConstraintsSet* cs = mCLInterface->mEffect->GetConstraintsSet(constraintIndex);
	cl_int clNumOctets = cs->GetNbConstraintOctets();
	mCLInterface->SetKernelArg(mKernel, 6, sizeof(clNumOctets), &clNumOctets);
	

	printf("General solver: kernel = %d, constraintIndex = %d\n", mKernel.mIndex, constraintIndex);
	int kernelToDebug = -1;


	if(mKernel.mIndex == kernelToDebug) {
		FILE  *myfile;
		myfile = fopen("Dump_values2.txt","w");
		fprintf(myfile, "numoctets=%d\n", clNumOctets);
		fprintf(myfile, "\n");
		fclose(myfile);

		myfile = fopen("Dump_octets.txt", "w");
		ROPA::ConstraintOctet* ConstraintOctet_dump = cs->GetConstraintsArray();
		for(unsigned int i=0; i<clNumOctets; i++) {
			for(unsigned int k=0; k<8; k++)
				fprintf(myfile, "%.14f ", ConstraintOctet_dump[i].mMax[k]);
			for(unsigned int k=0; k<8; k++)
				fprintf(myfile, "%.14f ", ConstraintOctet_dump[i].mMin[k]);
			for(unsigned int k=0; k<8; k++)
				fprintf(myfile, "%hi ", ConstraintOctet_dump[i].mParticleIdx[k]);
			for(unsigned int k=0; k<8; k++)
				fprintf(myfile, "%hi ", ConstraintOctet_dump[i].mRefObjIdx[k]);
			fprintf(myfile, "\n");
		}
		fprintf(myfile, "");
		fclose(myfile);
	}


	return mCLInterface->InvokeKernel(mKernel, event, true);
}

void opencl_GenericSolver::Init (RopaOpenCL *rcl, int kernelindex, const char* tName, const char* sName, const char* vName)
{
	mCLInterface = rcl;
	mKernel.Create(rcl->mProgram.mProgram, kernelindex, tName, sName, vName);
}


EventID opencl_DriverSolver::operator() (EventID event, int constraintIndex, int numOctets, int numIters, short refObjIdx)
{
	// InvokeKernel sets the first 5 parameters, opencl_GenericSolver::operator() sets the next 2
	mCLInterface->SetKernelBufferArg(mKernel, 7, mCLInterface->mCLDrivers);
	return opencl_GenericSolver::operator()(event, constraintIndex, numOctets, numIters, refObjIdx);
}

EventID opencl_SphereSolver::operator() (EventID event, int constraintIndex, int numOctets, int numIters, short refObjIdx)
{
	// InvokeKernel sets the first 5 parameters, opencl_GenericSolver::operator() sets the next 2
	mCLInterface->SetKernelBufferArg(mKernel, 7, mCLInterface->mCLVolumes);
	return opencl_GenericSolver::operator()(event, constraintIndex, numOctets, numIters, refObjIdx);
}
	
EventID opencl_TubeSolver::operator() (EventID event, int constraintIndex, int numOctets, int numIters, short refObjIdx)
{
	// InvokeKernel sets the first 5 parameters, opencl_GenericSolver::operator() sets the next 2
	mCLInterface->SetKernelBufferArg(mKernel, 7, mCLInterface->mCLVolumes);
	return opencl_GenericSolver::operator()(event, constraintIndex, numOctets, numIters, refObjIdx);
}


void opencl_LockBuffers::LockBuffers ()
{
	mCLInterface->mCLParticles.Map(mCLInterface->mCmdQs[mCLInterface->mCurrentDevice].mCmdQ);
	mCLInterface->mCLParticlesDebug.Map(mCLInterface->mCmdQs[mCLInterface->mCurrentDevice].mCmdQ);
	
#if !CL_GL_INTEROP_WORKING	
	mCLInterface->mOutputBuffers[mCLInterface->mCurrentOutput].Map(mCLInterface->mCmdQs[mCLInterface->mCurrentDevice].mCmdQ);
#endif
}

void opencl_LockBuffers::UnlockBuffers ()
{
	mCLInterface->mCLParticles.Unmap(mCLInterface->mCmdQs[mCLInterface->mCurrentDevice].mCmdQ);
	mCLInterface->mCLParticlesDebug.Unmap(mCLInterface->mCmdQs[mCLInterface->mCurrentDevice].mCmdQ);
	
#if !CL_GL_INTEROP_WORKING
	mCLInterface->mOutputBuffers[mCLInterface->mCurrentOutput].Unmap(mCLInterface->mCmdQs[mCLInterface->mCurrentDevice].mCmdQ);
#endif
}


//------------------------------------------------------------------------------------------------------------------------------------------------------------------------

RopaHarness::RopaHarness (void* glContext, const char* opencl_code, DistanceSolverOptions distanceSolverOpts, int local_ws, bool enableOutputDump) :
	mConstructData(NULL), 
	mBinaryData(NULL), mBinaryDataSize(0),
	mHeader(NULL), mNeedDelete(false),
	mRopaEffect(NULL), mVertexTransferBuffer(NULL), mVertexSourcePose(NULL),
	mShapeSize(10.0f), mCurrentDrawVerts(0), mMappedVerts(false), 
	mWaitAfterCompute(false), mHaveBuffers(false), mHaveUserImpulse(false),
	mUseOpenCL(false), mKernelMode(0), mUseGPU(false), mCLInterface(NULL), mEnableOutputDump(enableOutputDump)
{
	mCLInterface = new RopaOpenCL();
	mCLInterface->distanceSolverOpts = distanceSolverOpts;
	mCLInterface->local_ws = local_ws;
	mCLInterface->mProgram.distanceSolverOpts = distanceSolverOpts;
	mCLInterface->Init(glContext, opencl_code);
	this->SetMode(mUseOpenCL, mKernelMode, mUseGPU, false, false);

	this->ResetCamera();
	mUserImpulse[0] = 0.0f;
	mUserImpulse[1] = 0.0f;
	mObjectRotation[0] = mObjectRotation[1] = mObjectRotation[3] = 0.0f;
	mObjectRotation[2] = 1.0f;
}

void RopaHarness::DestructLoadData ()
{
	if (mMappedVerts)
		glUnmapBuffer(GL_ARRAY_BUFFER_ARB);
	
	if (mHaveBuffers)
	{
		glDeleteBuffers(2, mVertices);
		glDeleteBuffers(1, &mNormals);
		glDeleteBuffers(1, &mIndices);
	}
	
	delete mConstructData;
	delete mRopaEffect;
	delete [] mVertexTransferBuffer;
	delete [] mVertexSourcePose;
}

RopaHarness::~RopaHarness ()
{
	delete mCLInterface;
	mCLInterface = NULL;

	this->DestructLoadData();
	delete [] mBinaryData;
	if (mNeedDelete)
		delete mHeader;
	
	// prevent dead stripping
	CLPrintf("deadstripsuppress\n");
	CLBreakpoint(0);
}
	
void RopaHarness::ComputePoseAndCentroid ()
{
	uint16_t* reversemapping = (uint16_t*)(mBinaryData + mHeader->mReverseMapOffset);
	ROPA::Particle* particles = (ROPA::Particle*)(mBinaryData+mHeader->mParticlesOffset);
	mCentroid = Vector4();
	for (int i = 0; i < mHeader->mNumVerts; ++i)
 	{
		mVertexSourcePose[i] = particles[reversemapping[i]].mPos;
		mCentroid += mVertexSourcePose[i];
		*(uint32_t*)&mVertexSourcePose[i].w = ~0U;
	}
	
	mCentroid /= mHeader->mNumVerts;
	
	mShapeSize = 0.0f;
	for (int i = 0; i < mHeader->mNumVerts; ++i)
	{
		mVertexSourcePose[i] -= mCentroid;
		float mag = Magnitude(mVertexSourcePose[i]);
		if (mag > mShapeSize)
			mShapeSize = mag;
	}
	mShapeSize *= 5.0f;
}

void RopaHarness::ComputeNormalsFromPose ()
{
	uint16_t* indices = (uint16_t*)(mBinaryData + mHeader->mVertexIndicesOffset);

	// compute normals
	for (int i = 0; i < mHeader->mNumVerts; ++i)
	{
		mVertexTransferBuffer[i] = Vector4();
		for (int t = 0; t < mHeader->mNumTris; ++t)
		{
			bool isA = (indices[t*3+0] == i);
			bool isB = (indices[t*3+1] == i);
			bool isC = (indices[t*3+2] == i);
			if (isA || isB || isC)
			{
				Vector4 a = mVertexSourcePose[indices[t*3+0]];
				Vector4 b = mVertexSourcePose[indices[t*3+1]];
				Vector4 c = mVertexSourcePose[indices[t*3+2]];
				
				Vector4 d1, d2;
				if (isA)
				{
					d1 = b; d1 -= a;
					d2 = a; d2 -= c;
				}
				else if (isB)
				{
					d1 = b; d1 -= a;
					d2 = b; d2 -= c;
				}
				else
				{
					d1 = a; d1 -= c;
					d2 = c; d2 -= b;
				}
				
				mVertexTransferBuffer[i] += CrossProduct(Normalize(d1), Normalize(d2));
			}
		}
		
		mVertexTransferBuffer[i] = Normalize(mVertexTransferBuffer[i]);
	}
}

void RopaHarness::Load (const char* filename)
{
	delete [] mBinaryData;
	if (mNeedDelete)
		delete mHeader;
	mBinaryData = NULL;
	mHeader = NULL;
	
	//---------------------------------------------------------------------------------------------
	// load data from disk....
	FILE* fd = fopen(filename, "rb");
	fseek(fd, 0, SEEK_END);
	mBinaryDataSize = ftell(fd);
	fseek(fd, 0, SEEK_SET);
	mBinaryData = new char[mBinaryDataSize];
	fread(mBinaryData, sizeof(char), mBinaryDataSize, fd);
	fclose(fd);
	
	mNeedDelete = false;
	mHeader = (RopaBinaryHeader*)mBinaryData;
	
	mNumTris = mHeader->mNumTris;
	mNumAllocVerts = (mHeader->mNumVerts+3)&~3;		// want multiple of 4 verts allocated
	
	mNumFrames = mHeader->mNumFrames;
	mFrames = (Matrix44*)(mBinaryData+mHeader->mFramesOffset);
	mFrameTimes = (float*)(mBinaryData+mHeader->mFrameTimesOffset);
	mCurrentFrame = 0;

	//---------------------------------------------------------------------------------------------
	// release data allocated for previous load...
	mCLInterface->ReleaseData();
	this->DestructLoadData();

	//---------------------------------------------------------------------------------------------
	// begin allocating and finding data for the new load....
	mRopaEffect = new ROPA::Effect();
	mRopaEffect->Load(*mHeader, mBinaryData);
	
	#if HAS_GL_DEVICE
	glGenBuffersARB(1, &mNormals);
	glGenBuffersARB(1, &mIndices);
	glGenBuffersARB(2, mVertices);
	#endif
	mHaveBuffers = true;

	mVertexTransferBuffer = new Vector4[mNumAllocVerts];
	mVertexSourcePose = new Vector4[mNumAllocVerts];

	this->SetMode(mUseOpenCL, mKernelMode, mUseGPU, false, false);
	this->ComputePoseAndCentroid();
	this->ComputeNormalsFromPose();
	
	//---------------------------------------------------------------------------------------------
	// initialize gl buffers from original data...
	#if HAS_GL_DEVICE
	glBindBuffer(GL_ARRAY_BUFFER_ARB, mVertices[0]);
	glBufferDataARB(GL_ARRAY_BUFFER_ARB, sizeof(Vector4) * mNumAllocVerts, mVertexSourcePose, GL_STREAM_DRAW_ARB);

	glBindBuffer(GL_ARRAY_BUFFER_ARB, mVertices[1]);
	glBufferDataARB(GL_ARRAY_BUFFER_ARB, sizeof(Vector4) * mNumAllocVerts, mVertexSourcePose, GL_STREAM_DRAW_ARB);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, mIndices);
	glBufferDataARB(GL_ELEMENT_ARRAY_BUFFER_ARB, sizeof(uint16_t) * mHeader->mNumTris * 3, (uint16_t*)(mBinaryData + mHeader->mVertexIndicesOffset), GL_STATIC_DRAW_ARB);
	
	glBindBuffer(GL_ARRAY_BUFFER_ARB, mNormals);
	glBufferDataARB(GL_ARRAY_BUFFER_ARB, sizeof(Vector4) * mNumAllocVerts, mVertexTransferBuffer, GL_STATIC_DRAW_ARB);

	glBindBuffer(GL_ARRAY_BUFFER_ARB, 0);
	#endif
	
	mRopaEffect->UpdateTuningParameters();
	mCLInterface->SetupData(mRopaEffect, mVertexSourcePose, mVertices, mHeader->mOctetGroupSize);
	
	mHaveUserImpulse = true;
	this->ComputeFrame(0);
}

void* RopaHarness::GetSaveData () const
{
	return mBinaryData;
}

size_t RopaHarness::GetSaveDataSize () const
{
	return mBinaryDataSize;
}

void RopaHarness::SetMode(bool opencl, int kernelmode, bool usegpu, bool profiling, bool usemixedkernelmode)
{
#if INDIVIDUAL_KERNEL_TIMING
	if (mUseOpenCL && (mRopaEffect != NULL))
		printf("Average call times for kernel=%s, processor=%s:  %f    %f    %f    %f\n",
			   kKernelModeNames[mKernelMode], mUseGPU?"GPU":"cpu", 
			   mCLInterface->mIntegrator.mKernel.AverageDuration(), 
			   mCLInterface->mDriverSolver.mKernel.AverageDuration(), 
			   mCLInterface->mDistanceSolver.mKernel.AverageDuration(), 
			   mCLInterface->mWriteBack.mKernel.AverageDuration());
#endif	
	
	printf("SetMode(%s, kernel=%s, processor=%s)\n", opencl?"OpenCL":"host", kKernelModeNames[kernelmode], usegpu?"GPU":"cpu");
	
	mUseOpenCL = opencl;
	mKernelMode = kernelmode;
	mUseGPU = usegpu;
	
	mCLInterface->mKernelMode = kernelmode;
	mCLInterface->mCurrentDevice = usegpu ? mCLInterface->mGPU : mCLInterface->mCPU;
	mCLInterface->ProfileEventStream(profiling);
	
	if (usemixedkernelmode)
	{
		mCLInterface->mIndividualKernelModes[mCLInterface->mCPU][ROPA::kNumSolvers+0] = 0;
		mCLInterface->mIndividualKernelModes[mCLInterface->mCPU][ROPA::kDriverSolver] = 2;
		mCLInterface->mIndividualKernelModes[mCLInterface->mCPU][ROPA::kParticleDistanceSolver] = 2;
		mCLInterface->mIndividualKernelModes[mCLInterface->mCPU][ROPA::kNumSolvers+1] = 0;

		mCLInterface->mIndividualKernelModes[mCLInterface->mGPU][ROPA::kNumSolvers+0] = 2;
		mCLInterface->mIndividualKernelModes[mCLInterface->mGPU][ROPA::kDriverSolver] = 2;
		mCLInterface->mIndividualKernelModes[mCLInterface->mGPU][ROPA::kParticleDistanceSolver] = 1;
		mCLInterface->mIndividualKernelModes[mCLInterface->mGPU][ROPA::kNumSolvers+1] = 2;
	}
	else
		for (int d = 0; d < kMaxDevices; ++d)
			for (int k = 0; k < kMaxKernels; ++k)
				mCLInterface->mIndividualKernelModes[d][k] = kernelmode;


	if (mRopaEffect != NULL)
	{
		int kernelindextable[4] = { ROPA::kNumSolvers+0, ROPA::kNumSolvers+1, ROPA::kParticleDistanceSolver, ROPA::kDriverSolver };
		printf("workgroup sizes:  ");
		for (int k = 0; k < 4; ++k)
			printf("%ld,%ld    ", 
				   mCLInterface->mGlobalWorkSize[kernelindextable[k]][kernelmode][usegpu?mCLInterface->mGPU:mCLInterface->mCPU],
				   mCLInterface->mLocalWorkSize[kernelindextable[k]][kernelmode][usegpu?mCLInterface->mGPU:mCLInterface->mCPU]);
		printf("\n");
		
		if (opencl)
		{
			mRopaEffect->SetLockBuffersFunc(&mCLInterface->mLockBuffers);
#if USE_CL_INTEGRATOR
			mRopaEffect->SetIntegratorFunc(&mCLInterface->mIntegrator);
#endif
#if USE_CL_WRITEBACK
			mRopaEffect->SetWriteBackFunc(&mCLInterface->mWriteBack);
#endif
#if USE_CL_DISTANCE
			mRopaEffect->SetConstraintSolver(mRopaEffect->GetDistanceConstraintsIndex(), &mCLInterface->mDistanceSolver);
#endif
#if USE_CL_DRIVER
			mRopaEffect->SetConstraintSolver(mRopaEffect->GetDriverConstraintsIndex(), &mCLInterface->mDriverSolver);
#endif
		}
		else
		{
			mRopaEffect->SetLockBuffersFunc(NULL);
			mRopaEffect->SetIntegratorFunc(NULL);
			mRopaEffect->SetWriteBackFunc(NULL);
			mRopaEffect->SetConstraintSolver(mRopaEffect->GetDistanceConstraintsIndex(), NULL);
			mRopaEffect->SetConstraintSolver(mRopaEffect->GetDriverConstraintsIndex(), NULL);
		}
	}
}

void RopaHarness::UserImpulse (float x, float y)
{
	mHaveUserImpulse = true;
	mObjectRotation[0] += x;
	if (mRopaEffect != NULL)
	{
		mRopaEffect->SetGravity(y*0.25f);
		//printf("rotation=%f, gravity=%f\n", mObjectRotation[0], mRopaEffect->GetGravity());
	}
}

void RopaHarness::ResetCurrentFrame ()
{
	mCurrentFrame = 0;
}

void RopaHarness::ComputeFrame ( int cycle )
{
	if (mHaveUserImpulse)
	{		
		static Vector4 scale(1.0f,1.0f,1.0f,1.0f);
		
		mCurrentComputeBuffer = (mCurrentDrawVerts+1)&1;
		mCLInterface->mCurrentOutput = mCurrentComputeBuffer;

		if (mUseOpenCL)
			mCLInterface->BeginEventStream();
		
		// acquire the vertex buffer for output...
      #if HAS_GL_DEVICE
		glBindBuffer(GL_ARRAY_BUFFER, mVertices[mCurrentComputeBuffer]);
		void* output_buffer = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
      #else
      void* output_buffer = malloc(sizeof(Vector4) * mNumAllocVerts); 
      #endif
		if (mUseOpenCL || (output_buffer != NULL))
		{
			mRopaEffect->SetDestinationVertices(output_buffer, sizeof(Vector4), Vector4());

			Matrix44 framepos = mFrames[mCurrentFrame];
			float dt = mFrameTimes[mCurrentFrame];
			mCurrentFrame = (mCurrentFrame+1)%mNumFrames;

			// perform calculation of not OpenCL, initiate asynchronous series of computation if using OpenCL (forcing waits if mixed)
			EventID event = mRopaEffect->Update(EventID(), dt, framepos);

			// note that because we have vertices locked until here, an OpenCL write back operation cannot begin until we unlock it...
         #if HAS_GL_DEVICE
			if (!glUnmapBuffer(GL_ARRAY_BUFFER))
			{
				GLenum err = glGetError();
            const char *errorStr = "NA"; 
            // const char *errorStr = gluErrorString(err);
				printf("unmapping error = 0x%x, %s   buffer=%d index=%d\n", err, errorStr, mCurrentComputeBuffer, mVertices[mCurrentComputeBuffer]);
			}
         #endif
			
			if (mUseOpenCL)
			{
				if (mWaitAfterCompute)
				{	// wait for asynchronous computations to complete...
					event.Wait();
				}
				
#if !CL_GL_INTEROP_WORKING
				//--------------------------------------------
				// CL/GL interop not working, copy manually...
            #if HAS_GL_DEVICE
				output_buffer = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
            #else
            output_buffer = NULL; 
            #endif
				printf("Output cycle=%d\n", cycle);
				char * mode;
				if(cycle == 0)
					mode = (char *) "w";
				else
					mode = (char *) "a";

				if (output_buffer != NULL)
				{
					event = mCLInterface->ReadFromBuffer(event, mCLInterface->mOutputBuffers[mCurrentComputeBuffer], output_buffer, 0, mHeader->mNumVerts * 16);
					event.Wait();

					printf("GL Enabled.\n");

					if(mEnableOutputDump) {
					    printf("Dump output to file.\n");
					    Print_Memory2("output_dump_gl.txt", mode, (unsigned char*) output_buffer, mHeader->mNumVerts * 16);

					    // Dump vertex values
                        printf("Dump vertices values to file. # of vertices = %d\n", mHeader->mNumVerts);
                        FILE  *myfile;
                        myfile = fopen("output_vertices_gl.txt", mode);
                        fprintf(myfile, "Cycle %d\n", cycle);
                        float* Vertices_dump = (float*)output_buffer;
                        for(unsigned int i=0; i<mHeader->mNumVerts; i++){
                            fprintf(myfile, "%.14f %.14f %.14f %u ", Vertices_dump[i*4], Vertices_dump[i*4+1], Vertices_dump[i*4+2], (unsigned int) Vertices_dump[i*4+3]);
                            fprintf(myfile, "\n");
                        }
                        fclose(myfile);
					}

					//printf("<====================OutputBuffer ( dest ) ==========>");
					//printf("Dumping output buffer to file");
					//Print_Memory((unsigned char*)output_buffer,(mHeader->mNumVerts * 16));

					glUnmapBuffer(GL_ARRAY_BUFFER);
				} else {
					printf("GL Disabled.\n");
					cl_command_queue q = mCLInterface->mCmdQs[mCLInterface->mCurrentDevice].mCmdQ;
					unsigned char* output_dump = (unsigned char*) malloc(mHeader->mNumVerts * 16);
					cl_int err = clEnqueueReadBuffer(q, mCLInterface->mOutputBuffers[mCurrentComputeBuffer].mBuffer, true, 0, mHeader->mNumVerts * 16, output_dump, 0, NULL, NULL);

					if(mEnableOutputDump) {
					    printf("Dump output to file.\n");
					    Print_Memory2("output_dump.txt", mode, output_dump, mHeader->mNumVerts * 16);

					    // Dump vertex values
                        printf("Dump vertices values to file. # of vertices = %d\n", mHeader->mNumVerts);
                        FILE  *myfile;
                        myfile = fopen("output_vertices.txt", mode);
                        fprintf(myfile, "Cycle %d\n", cycle);
                        float* Vertices_dump = (float*)output_dump;
                        for(unsigned int i=0; i<mHeader->mNumVerts; i++){
                            fprintf(myfile, "%.14f %.14f %.14f %u ", Vertices_dump[i*4], Vertices_dump[i*4+1], Vertices_dump[i*4+2], (unsigned int) Vertices_dump[i*4+3]);
                            fprintf(myfile, "\n");
                        }
                        fclose(myfile);
					}

				}
#endif
			}
			else
			{
				// the non-opencl write back doesn't copy colours for us, so we copy them here
				#if HAS_GL_DEVICE
				output_buffer = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
				#endif
				if (output_buffer != NULL)
				{
					uint16_t* reversemapping = (uint16_t*)(mBinaryData + mHeader->mReverseMapOffset);
					uint32_t* colours = (uint32_t*)output_buffer;
					const ROPA::ParticleDebug* pd = mRopaEffect->GetParticlesDebug();
					for (int i = 0; i < mHeader->mNumVerts; ++i)
						colours[i*4+3] = pd[reversemapping[i]].mColor;		// note that this is a combine position / colour buffer  XYZC
					#if HAS_GL_DEVICE
					glUnmapBuffer(GL_ARRAY_BUFFER);
					#endif
				}
			}
				
			// tell rendering to use the newly computed buffer...
			mCurrentDrawVerts = mCurrentComputeBuffer;
		}
		else
		{
			GLenum err = glGetError();
			const char *errorStr = "NA"; 
			// const char *errorStr = gluErrorString(err);
			printf("mapping error = 0x%x, %s   buffer=%d index=%d\n", err, errorStr, mCurrentComputeBuffer, mVertices[mCurrentComputeBuffer]);
		}
		
      #if !HAS_GL_DEVICE
      free(output_buffer);
      #endif
		if (mUseOpenCL)
			mCLInterface->EndEventStream();
//		mHaveUserImpulse = false;
	}
}

void RopaHarness::ResetCamera ()
{
	mCamera.aperture = 5;
	mCamera.rotPoint = gOrigin;
	
	mCamera.viewPos.x = 0.0;
	mCamera.viewPos.y = 9.0;
	mCamera.viewPos.z = -25.0;
//	mCamera.viewDir.x = -mCamera.viewPos.x; 
//	mCamera.viewDir.y = -mCamera.viewPos.y; 
//	mCamera.viewDir.z = -mCamera.viewPos.z;
	mCamera.viewDir.x = mCentroid.x - mCamera.viewPos.x; 
	mCamera.viewDir.y = mCentroid.y - mCamera.viewPos.y;
	mCamera.viewDir.z = mCentroid.z - mCamera.viewPos.z;
	mCamera.viewUp.x = 0;  
	mCamera.viewUp.y = 1; 
	mCamera.viewUp.z = 0;
}

void RopaHarness::RotateObject (float angle, float x, float y, float z)
{
	if (mRopaEffect == NULL)
	{
		mObjectRotation[0] = angle;
		mObjectRotation[1] = x;
		mObjectRotation[2] = y;
		mObjectRotation[3] = z;
	}
}

void RopaHarness::SetViewport (float width, float height)
{
	mCamera.viewHeight = height;
	mCamera.viewWidth = width;
}

void RopaHarness::SetCamera () const
{
#if HAS_GL_DEVICE
	GLdouble ratio, radians, wd2;
	GLdouble left, right, top, bottom, Near, Far;

	glViewport (0, 0, mCamera.viewWidth, mCamera.viewHeight);

	// set projection
	glMatrixMode (GL_PROJECTION);
	glLoadIdentity ();
	Near = -mCamera.viewPos.z - mShapeSize;		//* 0.5;
	if (Near < 0.00001)
		Near = 0.00001;
	Far = -mCamera.viewPos.z + mShapeSize;		//* 0.5;
	if (Far < 1.0)
		Far = 1.0;
	radians = 0.0174532925 * mCamera.aperture / 2; // half aperture degrees to radians 
	wd2 = Near * tan(radians);
	ratio = mCamera.viewWidth / (float) mCamera.viewHeight;
	if (ratio >= 1.0) {
		left  = -ratio * wd2;
		right = ratio * wd2;
		top = wd2;
		bottom = -wd2;	
	} else {
		left  = -wd2;
		right = wd2;
		top = wd2 / ratio;
		bottom = -wd2 / ratio;	
	}
	glFrustum (left, right, bottom, top, Near, Far);
	
	glMatrixMode (GL_MODELVIEW);
	glLoadIdentity ();
	gluLookAt (mCamera.viewPos.x, mCamera.viewPos.y, mCamera.viewPos.z,
			   mCentroid.x, mCentroid.y, mCentroid.z,
			   //mCamera.viewPos.x + mCamera.viewDir.x,
			   //mCamera.viewPos.y + mCamera.viewDir.y,
			   //mCamera.viewPos.z + mCamera.viewDir.z,
			   mCamera.viewUp.x, mCamera.viewUp.y ,mCamera.viewUp.z);
#endif
}

void RopaHarness::SetLighting () const
{
	// set up light colors (ambient, diffuse, specular)
	GLfloat lightKa[] = {.2f, .2f, .2f, 1.0f};  // ambient light
	GLfloat lightKd[] = {.7f, .7f, .7f, 1.0f};  // diffuse light
	GLfloat lightKs[] = {1, 1, 1, 1};           // specular light
	glLightfv(GL_LIGHT0, GL_AMBIENT, lightKa);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, lightKd);
	glLightfv(GL_LIGHT0, GL_SPECULAR, lightKs);
	
	// position the light
	float lightPos[4] = {0, 0, 10, 1}; // positional light
	glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
	glEnable(GL_LIGHT0);                        // MUST enable each light source after configuration
}

GLint cube_num_vertices = 8;

GLfloat cube_vertices [8][3] = {
	{1.0, 1.0, 1.0}, {1.0, -1.0, 1.0}, {-1.0, -1.0, 1.0}, {-1.0, 1.0, 1.0},
	{1.0, 1.0, -1.0}, {1.0, -1.0, -1.0}, {-1.0, -1.0, -1.0}, {-1.0, 1.0, -1.0} };

GLfloat cube_vertex_colors [8][3] = {
	{1.0, 1.0, 1.0}, {1.0, 1.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 1.0, 1.0},
	{1.0, 0.0, 1.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0} };

GLint num_faces = 6;

short cube_faces [6][4] = { {3, 2, 1, 0}, {2, 3, 7, 6}, {0, 1, 5, 4}, {3, 0, 4, 7}, {1, 2, 6, 5}, {4, 5, 6, 7} };

void RopaHarness::Render () const
{
#if !HAS_GL_DEVICE
   return; 
#endif
	glShadeModel(GL_SMOOTH);                    // shading mathod: GL_SMOOTH or GL_FLAT
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_LIGHTING);
	glEnable(GL_TEXTURE_2D);
//	glEnable(GL_CULL_FACE);
//	glFrontFace(GL_CCW);

	glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
	glEnable(GL_COLOR_MATERIAL);
	glDepthFunc(GL_LEQUAL);
	
	this->SetCamera();
	this->SetLighting();

	glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

	if (mRopaEffect != NULL)
	{
		//printf("%c", mCurrentDrawVerts?'/':'\\');
		
		glDisable(GL_BLEND);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_NORMAL_ARRAY);
		glEnableClientState(GL_INDEX_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);
	
		glBindBuffer(GL_ARRAY_BUFFER_ARB, mVertices[mCurrentDrawVerts]);
		glVertexPointer(3, GL_FLOAT, 16, 0);			// buffer contains XYZC where C is a 4 byte colour
		glColorPointer(4, GL_UNSIGNED_BYTE, 16, (GLvoid*)12);

		glBindBuffer(GL_ARRAY_BUFFER_ARB, mNormals);
		glNormalPointer(GL_FLOAT, 4*sizeof(float), 0);
		
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		
		glTranslatef(mCentroid.x,mCentroid.y,mCentroid.z);
		glRotatef(mObjectRotation[0], mObjectRotation[1], mObjectRotation[2], mObjectRotation[3]);
		glTranslatef(-mCentroid.x,-mCentroid.y,-mCentroid.z);
	
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB, mIndices);
		glDrawElements(GL_TRIANGLES, mNumTris*3, GL_UNSIGNED_SHORT, 0);

		glPopMatrix();
		
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_NORMAL_ARRAY);
		glDisableClientState(GL_INDEX_ARRAY);
	}
	else
	{
		glRotatef(mObjectRotation[0], mObjectRotation[1], mObjectRotation[2], mObjectRotation[3]);

		//glEnable(GL_BLEND);
		//glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		
		GLfloat fSize = 0.25f;
		glBegin (GL_QUADS);
		for (GLint f = 0; f < num_faces; f++)
			for (GLint i = 0; i < 4; i++) {
				glColor4f (cube_vertex_colors[cube_faces[f][i]][0], cube_vertex_colors[cube_faces[f][i]][1], cube_vertex_colors[cube_faces[f][i]][2], 1.0f);
				glVertex3f(cube_vertices[cube_faces[f][i]][0] * fSize, cube_vertices[cube_faces[f][i]][1] * fSize, cube_vertices[cube_faces[f][i]][2] * fSize);
			}
		glEnd ();
	}	
}

void RopaHarness::PrepareToConstruct ()
{
	delete mConstructData;
	mConstructData = new RopaConstructionData();
	
	if (mNeedDelete)
		delete mHeader;
	mHeader = new RopaBinaryHeader();
	memset(mHeader, 0, sizeof(RopaBinaryHeader));
	mHeader->mOctetGroupSize = 1;
	mNeedDelete = true;	
}

void RopaHarness::AddReplication (float x, float y, float z)
{
	mConstructData->mReplications.push_back(Vector4(x,y,z,0));
}

template<typename TYPE> char* memcpy_and_update(char* dst, TYPE* src, int count)
{
	int bytes = sizeof(TYPE)*count;
	memcpy(dst, src, bytes);
	char* ptr = dst + bytes;
	intptr_t iPtr = (intptr_t)ptr;
	iPtr = (iPtr+255) & ~0xff;
	return (char*)iPtr;
}

void RopaHarness::InitEffect ()
{
	mConstructData->PerformReplications();
	
	mHeader->mNumParticles = mConstructData->mParticles.size();
	mHeader->mNumDrivers = mConstructData->mDrivers.size();
	mHeader->mNumVolumes = mConstructData->mVolumes.size();
	mHeader->mNumVertsMapped = mConstructData->mVertexMap.size();
	mHeader->mNumMappedVerts = mConstructData->mVertexMap.size();
	mHeader->mNumVerts = mConstructData->mReverseMap.size();
	mHeader->mNumTris = mConstructData->mVertIndices.size() / 3;	
	for (int cs = 0; cs < mHeader->mNumConstraintSets; ++cs)
		mHeader->mNumOctets[cs] = mConstructData->mOctets[cs].size();
	mHeader->mNumFrames = mConstructData->mFramePositions.size();
	mHeader->mOctetGroupSize = mConstructData->mReplications.size() + 1;
	
	// allocate binary data
	mBinaryDataSize = sizeof(RopaBinaryHeader) + sizeof(ROPA::Particle) * mHeader->mNumParticles + 
		sizeof(ROPA::PosNormPair) * mHeader->mNumDrivers + sizeof(ROPA::VolumeTransform) * mHeader->mNumVolumes + 
		(sizeof(Matrix44) + sizeof(float)) * mHeader->mNumFrames + 
		sizeof(uint32_t) * (mHeader->mNumMappedVerts) + 
		sizeof(uint16_t) * (mHeader->mNumVertsMapped+mHeader->mNumVerts+mHeader->mNumTris*3);
	for (int cs = 0; cs < mHeader->mNumConstraintSets; ++cs)
		mBinaryDataSize += sizeof(ROPA::ConstraintOctet) * mHeader->mNumOctets[cs];

	mBinaryDataSize += 32 * 256;		// padding!
	mBinaryData = new char[mBinaryDataSize];

	// move header into binary data
	char* dst = mBinaryData;
	dst = memcpy_and_update(dst, mHeader, 1);
	delete mHeader;
	mNeedDelete = false;
	mHeader = (RopaBinaryHeader*)mBinaryData;
	
	// copy in all array data
	mHeader->mParticlesOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mParticles.begin(), mHeader->mNumParticles);
	
	mHeader->mDriversOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mDrivers.begin(), mHeader->mNumDrivers);
	
	mHeader->mVolumesOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mVolumes.begin(), mHeader->mNumVolumes);
	
	for (int cs = 0; cs < mHeader->mNumConstraintSets; ++cs)
	{
		mHeader->mConstraintOctetOffsets[cs] = dst - mBinaryData;
		dst = memcpy_and_update(dst, &*mConstructData->mOctets[cs].begin(), mHeader->mNumOctets[cs]);
	}
	
	mHeader->mFramesOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mFramePositions.begin(), mHeader->mNumFrames);
	
	mHeader->mFrameTimesOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mFrameTimes.begin(), mHeader->mNumFrames);
	
	mHeader->mVertexMapOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mVertexMap.begin(), mHeader->mNumVertsMapped);
	
	mHeader->mMappedVerticesOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mMappedVertices.begin(), mHeader->mNumMappedVerts);
	
	mHeader->mReverseMapOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mReverseMap.begin(), mHeader->mNumVerts);
	
	mHeader->mVertexIndicesOffset = dst - mBinaryData;
	dst = memcpy_and_update(dst, &*mConstructData->mVertIndices.begin(), mHeader->mNumTris*3);
	
	mBinaryDataSize = dst - mBinaryData;
	
	delete mConstructData;
	mConstructData = NULL;
}

void RopaHarness::AddParticlePos (const Vector4& pos)
{
	mConstructData->mParticles.push_back(ROPA::Particle());
	mConstructData->mParticles.back().mPos = pos;
}

void RopaHarness::AddParticlePrevPos (const Vector4& pos)
{
	mConstructData->mParticles.back().mPrevPos = pos;
}


void RopaHarness::AddDriverPosition (const Vector4& pos)
{
	mConstructData->mDrivers.push_back(ROPA::PosNormPair());
	mConstructData->mDrivers.back().mPosition = pos;
}

void RopaHarness::AddDriverNormal (const Vector4& pos)
{
	mConstructData->mDrivers.back().mNormal = pos;
}

void RopaHarness::AddVolumeTransform (const Matrix44& m)
{
	mConstructData->mVolumes.push_back(ROPA::VolumeTransform());
	mConstructData->mVolumes.back().mTransform = m;
}

void RopaHarness::AddVolumeInverse (const Matrix44& m)
{
	mConstructData->mVolumes.back().mInvTransform = m;
}

void RopaHarness::AddVertexMapElement (int v)
{
	mConstructData->mVertexMap.push_back(v);
}

void RopaHarness::AddMappedVertexElement (int v)
{
	mConstructData->mMappedVertices.push_back((uint32_t)v);
}

void RopaHarness::AddReverseMappingElement (int v)
{
	mConstructData->mReverseMap.push_back(v);
}

void RopaHarness::AddVertexIndex(int v)
{
	mConstructData->mVertIndices.push_back(v);
}

void RopaHarness::AddConstraintsSet ()
{
	mHeader->mNumConstraintSets += 1;
}

void RopaHarness::AddConstraintOctet (const ROPA::ConstraintOctet& co)
{
	mConstructData->mOctets[mHeader->mNumConstraintSets-1].push_back(co);
}


void RopaHarness::SetConstraintsSetIterations (int iters)
{
	mHeader->mIterations[mHeader->mNumConstraintSets-1] = iters;
}

void RopaHarness::SetConstraintsSetSolver (int solver)
{
	mHeader->mSolvers[mHeader->mNumConstraintSets-1] = solver;
}

void RopaHarness::SetConstraintsSetObject (int index)
{
	mHeader->mObjects[mHeader->mNumConstraintSets-1] = index;
}


void RopaHarness::SetDT (float dt)
{
	mHeader->mDT = dt;
}

void RopaHarness::SetLastDT (float dt)
{
	mHeader->mLastDT = dt;
}

void RopaHarness::SetLastFullDT (float dt)
{
	mHeader->mLastFullDT = dt;
}

void RopaHarness::SetDistanceCDIndex (int i)
{
	mHeader->mDistanceCD = i;
}

void RopaHarness::SetDriverCDIndex (int i)
{
	mHeader->mDriverCD = i;
}

void RopaHarness::SetSystemPosition (const Matrix44& m)
{
	mHeader->mSystemPosition = m;
}

void RopaHarness::SetDeltaPos (const Matrix44& m)
{
	mHeader->mDeltaPos = m;
}

void RopaHarness::SetVertexPositionPackingOffset (const Vector4& v)
{
	mHeader->mVertexPositionPackingOffset = v;
}

void RopaHarness::SetGravity (const Vector4& v)
{
	mHeader->mGravity = v;
}

void RopaHarness::AddFramePosition (const Matrix44& m, float dt)
{
	mConstructData->mFramePositions.push_back(m);
	mConstructData->mFrameTimes.push_back(dt);
}

