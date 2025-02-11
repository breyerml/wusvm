/*
Copyright (c) 2014, Washington University in St. Louis
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Washington University in St. Louis nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
ARE DISCLAIMED. IN NO EVENT SHALL WASHINGTON UNIVERSITY BE LIABLE FOR ANY 
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND 
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF 
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "hessian.h"
#include "kernels.h"

void lasp::update_hess(svm_problem& p, LaspMatrix<double> HESS, LaspMatrix<double> K, vector<int>* erv, LaspMatrix<double> y, vector<int> &S, LaspMatrix<double>& x, LaspMatrix<double>& xNorm){
	bool gpu = p.options.usegpu;
	
	
	int d = S.size();
	int d0 = HESS.rows() - 1;
	int nerv = erv->size();
	
	if(d0+2 > d+1){
		cerr << "Dimension error in update_hess!" << endl;
	}
	
	int subK = (d+1)-(d0+1);
	
	LaspMatrix<double> h (subK,(d+1), 0.0);
	LaspMatrix<double> h_new;
	
	if(gpu){
		h.transferToDevice();
		h_new.transferToDevice();
	}
	
	if (p.options.smallKernel) {
		//kernel options struct for computing the kernel
		kernel_opt kernelOptions;
		kernelOptions.kernel = p.options.kernel;
		kernelOptions.gamma = p.options.gamma;
		kernelOptions.degree = p.options.degree;
		kernelOptions.coef = p.options.coef;
		
		{
			LaspMatrix<double> h_t = h(0,1,h.cols(), h.rows());
			LaspMatrix<double> h_kernel = compute_kernel(kernelOptions, x, xNorm, S.data(), S.size(), x, xNorm, S.data() + (d0), S.size() - (d0), gpu);
			h_t.copy(h_kernel);
		}
		
		{
			LaspMatrix<double> h_kernel(h.cols(), h.rows(), 0.0);
			
			int numChunks = nerv > p.options.set_size ? (nerv + p.options.set_size - 1) / p.options.set_size : min(nerv, 10);
			int chunkStart = 0;
			LaspMatrix<double> Kcpy(nerv / numChunks, d+1);
			
			if (gpu) {
				Kcpy.transferToDevice();
			}
			
			LaspMatrix<double> xS, xSNorm, xErv, xErvNorm;
			x.gather(xS, S);
			xNorm.gather(xSNorm, S);
			
			for (int i = 0; i < numChunks; ++i) {
				int chunkSize = (i == numChunks - 1) ? nerv - chunkStart : nerv / numChunks;
				
				Kcpy.resize(chunkSize, d+1);
				LaspMatrix<double> Kcpy_row_one = Kcpy(0, 0, Kcpy.cols(), 1);
				LaspMatrix<double> Kcpy_kernel = Kcpy(0, 1, Kcpy.cols(), Kcpy.rows());
				
				y.gather(Kcpy_row_one, erv->begin() + chunkStart, erv->begin() + chunkStart + chunkSize);
				
				{
					vector<int>::iterator csIterBegin = erv->begin() + chunkStart;
					vector<int>::iterator csIterEnd = erv->begin() + chunkStart + chunkSize;
					x.gather(xErv, csIterBegin, csIterEnd);
					xNorm.gather(xErvNorm, csIterBegin, csIterEnd);
					Kcpy_kernel.getKernel(kernelOptions, xS, xSNorm, xErv, xErvNorm, false, false, gpu);
					Kcpy_kernel.rowWiseMult(Kcpy_row_one);
				}
				
				LaspMatrix<double> h_kernel_temp;
				LaspMatrix<double>  norm1;
				LaspMatrix<double> norm2;
				LaspMatrix<double> Kcpy_mult = Kcpy(0, d0+1, Kcpy.cols(), Kcpy.rows());
				
				if (gpu) {
					h_kernel_temp.transferToDevice();
				}
				
				h_kernel_temp.getKernel(kernel_opt(), Kcpy, norm1, Kcpy_mult, norm2, true, true);
				h_kernel.add(h_kernel_temp);
				
				chunkStart += chunkSize;
			}
			
			h_kernel.multiply(p.options.C);
			h_kernel.add(h, h);
			
		}
		
	} else {
		LaspMatrix<double> K_new;
		LaspMatrix<double> h_trans = h(0,1,h.cols(), h.rows());
		
		K_new = K(0, d0+1, K.cols(), (d0+1+subK));
		K_new.gather(h_new, S);
		
		LaspMatrix<double> y_new;
		y.gather(y_new, S);
		h_new.rowWiseMult(y_new);
		h_new.transpose(h_trans);

		K_new = K(0, 0, K.cols(), d+1);
		LaspMatrix<double> Kcpy;
		LaspMatrix<int> contigifyMap;
		
		//Check that the big kernel copy is actually possible
		if (!p.options.contigify) {
			int error = 0;
			
			try {
				error = K_new.gather(Kcpy, *erv);
			} catch (...) {
				p.options.contigify = true;
			}
				
			if(error != 0) {
				p.options.contigify = true;
			}
		}
		
		//Contigify gathers erv columns into the kernel itself to
		//save memory
		if (p.options.contigify) {
			contigifyMap.resize(nerv, 1);
			for (int erv_i = 0; erv_i < nerv; ++erv_i) {
				contigifyMap(erv_i, 0) = (*erv)[erv_i];
			}
			
			K_new.contigify(contigifyMap);
			Kcpy = K_new(0, 0, nerv, d+1);
		}
		
		LaspMatrix<double> h_kernel;
		LaspMatrix<double> norm1;
		LaspMatrix<double> norm2;
		LaspMatrix<double> Kcpy_mult = Kcpy(0, d0+1, Kcpy.cols(), Kcpy.rows());
		
		if(gpu){
			h_kernel.transferToDevice();
		}
		
		h_kernel.getKernel(kernel_opt(), Kcpy, norm1, Kcpy_mult, norm2, true, true);
		h_kernel.multiply(p.options.C);
		
		h_kernel.add(h, h);
		
		
		//Revert the kernel to its original state
		if (p.options.contigify) {
			K_new.revert(contigifyMap);
		}
	}
		
	int count = 0;
	double mean = 0;
	for(count = 0; count < subK; ++count){
		mean += h(count, (d0+1) + count);
	}
	
	if (gpu){
		h.transferToDevice();
	}
	
	mean = (mean / count) * 1e-10;
	int oldDim = HESS.rows();
	HESS.resize(HESS.cols()+subK, HESS.rows()+subK);
	
	LaspMatrix<double> HESS_new = HESS(oldDim, 0, HESS.cols(), HESS.rows());
	HESS_new.copy(h);
	HESS_new = HESS(0, oldDim, HESS.cols(), HESS.rows());
	
	h_new = h(0, 0, h.cols(), oldDim);
	h_new.transpose(HESS_new);
	
	HESS_new = HESS(oldDim, oldDim, HESS.cols(), HESS.rows());
	HESS_new.add(mean);
	
}
