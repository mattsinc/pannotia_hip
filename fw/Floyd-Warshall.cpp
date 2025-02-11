/************************************************************************************\ 
 *                                                                                  *
 * Copyright © 2014 Advanced Micro Devices, Inc.                                    *
 * Copyright (c) 2015 Mark D. Hill and David A. Wood                                *
 * All rights reserved.                                                             *
 *                                                                                  *
 * Redistribution and use in source and binary forms, with or without               *
 * modification, are permitted provided that the following are met:                 *
 *                                                                                  *
 * You must reproduce the above copyright notice.                                   *
 *                                                                                  *
 * Neither the name of the copyright holder nor the names of its contributors       *
 * may be used to endorse or promote products derived from this software            *
 * without specific, prior, written permission from at least the copyright holder.  *
 *                                                                                  *
 * You must include the following terms in your license and/or other materials      *
 * provided with the software.                                                      *
 *                                                                                  *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"      *
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE        *
 * IMPLIED WARRANTIES OF MERCHANTABILITY, NON-INFRINGEMENT, AND FITNESS FOR A       *
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER        *
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,         *
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT  *
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS      *
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN          *
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING  *
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY   *
 * OF SUCH DAMAGE.                                                                  *
 *                                                                                  *
 * Without limiting the foregoing, the software may implement third party           *
 * technologies for which you must obtain licenses from parties other than AMD.     *
 * You agree that AMD has not obtained or conveyed to you, and that you shall       *
 * be responsible for obtaining the rights to use and/or distribute the applicable  *
 * underlying intellectual property rights related to the third party technologies. *
 * These third party technologies are not licensed hereunder.                       *
 *                                                                                  *
 * If you use the software (in whole or in part), you shall adhere to all           *
 * applicable U.S., European, and other export laws, including but not limited to   *
 * the U.S. Export Administration Regulations ("EAR"�) (15 C.F.R Sections 730-774),  *
 * and E.U. Council Regulation (EC) No 428/2009 of 5 May 2009.  Further, pursuant   *
 * to Section 740.6 of the EAR, you hereby certify that, except pursuant to a       *
 * license granted by the United States Department of Commerce Bureau of Industry   *
 * and Security or as otherwise permitted pursuant to a License Exception under     *
 * the U.S. Export Administration Regulations ("EAR"), you will not (1) export,     *
 * re-export or release to a national of a country in Country Groups D:1, E:1 or    *
 * E:2 any restricted technology, software, or source code you receive hereunder,   *
 * or (2) export to Country Groups D:1, E:1 or E:2 the direct product of such       *
 * technology or software, if such foreign produced direct product is subject to    *
 * national security controls as identified on the Commerce Control List (currently *
 * found in Supplement 1 to Part 774 of EAR).  For the most current Country Group   *
 * listings, or for additional information about the EAR or your obligations under  *
 * those regulations, please refer to the U.S. Bureau of Industry and Security's    *
 * website at http://www.bis.doc.gov/.                                              *
 *                                                                                  *
\************************************************************************************/

#include "hip/hip_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <sys/time.h>
//#include <omp.h>
#include "../graph_parser/util.h"
#include "kernel.h"
#include "parse.h"

#ifdef GEM5_FUSION
#include <stdint.h>
#include <gem5/m5ops.h>
#endif

#ifdef GEM5_FUSION
#define MAX_ITERS 192
#else
#include <stdint.h>
#define MAX_ITERS INT32_MAX
#endif

#define BIGNUM 999999
#define TRUE 1
#define FALSE 0

int main(int argc, char **argv)
{
    char *tmpchar;
    bool verify_results = false;

    int num_nodes;
    int num_edges;

    hipError_t err = hipSuccess;

    // Get program input
    if (argc >= 2) {
        tmpchar = argv[1];  // Graph input file
    } else {
        fprintf(stderr, "You did something wrong!\n");
        exit(1);
    }

    if (argc >= 3) {
        if (atoi(argv[2]) == 1) {
            verify_results = true;
        }
    }

    // Parse the adjacency matrix
    int *adjmatrix = parse_graph_file(&num_nodes, &num_edges, tmpchar);
    int dim = num_nodes;

    // Initialize the distance matrix
    int *distmatrix = (int *)malloc(dim * dim * sizeof(int));
    if (!distmatrix) fprintf(stderr, "malloc failed - distmatrix\n");

    // Initialize the result matrix
    int *result = (int *)malloc(dim * dim * sizeof(int));
    if (!result) fprintf(stderr, "malloc failed - result\n");

    // TODO: Now only supports integer weights
    // Setup the input matrix
    for (int i = 0 ; i < dim; i++) {
        for (int j = 0 ; j < dim; j++) {
            if (i == j) {
                // Diagonal
                distmatrix[i * dim + j] = 0;
            } else if (adjmatrix[i * dim + j] == -1) {
                // Without edge
                distmatrix[i * dim + j] = BIGNUM;
            } else {
                // With edge
                distmatrix[i * dim + j] = adjmatrix[i * dim + j];
            }
        }
    }

    int *dist_d;
    int *next_d;

    // Create device-side FW buffers
    err = hipMalloc(&dist_d, dim * dim * sizeof(int));
    if (err != hipSuccess) {
        printf("ERROR: hipMalloc dist_d (size:%d) => %d\n",  dim * dim , err);
        return -1;
    }
    err = hipMalloc(&next_d, dim * dim * sizeof(int));
    if (err != hipSuccess) {
        printf("ERROR: hipMalloc next_d (size:%d) => %d\n",  dim * dim , err);
        return -1;
    }

    //double timer1 = gettime();

#ifdef GEM5_FUSION
    m5_work_begin(0, 0);
#endif

    // Copy the dist matrix to the device
    err = hipMemcpy(dist_d, distmatrix, dim * dim * sizeof(int), hipMemcpyHostToDevice);
    if (err != hipSuccess) {
        fprintf(stderr, "ERROR: hipMemcpy feature_d (size:%d) => %d\n", dim * dim, err);
        return -1;
    }

    // Work dimension
    dim3 threads(16, 16, 1);
    dim3 grid(num_nodes / 16, num_nodes / 16, 1);

    //double timer3 = gettime();
    // Main computation loop
    for (int k = 1; k < dim && k < MAX_ITERS; k++) {
        hipLaunchKernelGGL(HIP_KERNEL_NAME(floydwarshall), dim3(grid), dim3(threads), 0, 0, dist_d, next_d, dim, k);
    }
    hipDeviceSynchronize();

    //double timer4 = gettime();
    err = hipMemcpy(result, dist_d, dim * dim * sizeof(int), hipMemcpyDeviceToHost);
    if (err != hipSuccess) {
        fprintf(stderr, "ERROR:  read back dist_d %d failed\n", err);
        return -1;
    }

#ifdef GEM5_FUSION
    m5_work_end(0, 0);
#endif

    //double timer2 = gettime();

    //printf("kernel time = %lf ms\n", (timer4 - timer3) * 1000);
    //printf("kernel + memcpy time = %lf ms\n", (timer2 - timer1) * 1000);

    if (verify_results) {
        // Below is the verification part
        // Calculate on the CPU
        int *dist = distmatrix;
        for (int k = 0; k < dim; k++) {
            for (int i = 0; i < dim; i++) {
                for (int j = 0; j < dim; j++) {
                    if (dist[i * dim + k] + dist[k * dim + j] < dist[i * dim + j]) {
                        dist[i * dim + j] = dist[i * dim + k] + dist[k * dim + j];
                    }
                }
            }
        }

        // Compare results
        bool check_flag = 0;
        for (int i = 0; i < dim; i++) {
            for (int j = 0; j < dim; j++) {
                if (dist[i * dim + j] !=  result[i * dim + j]) {
                    fprintf(stderr, "mismatch at (%d, %d)\n", i, j);
                    check_flag = 1;
                }
            }
        }
        // If there is mismatch, report
        if (check_flag) {
            fprintf(stdout, "WARNING: Produced incorrect results!\n");
        } else {
            printf("Results are correct!\n");
        }
    }

    printf("Finishing Floyd-Warshall\n");

    // Free host-side buffers
    free(adjmatrix);
    free(result);
    free(distmatrix);

    // Free CUDA buffers
    hipFree(dist_d);
    hipFree(next_d);

    return 0;
}
