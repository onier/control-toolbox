/***********************************************************************************
Copyright (c) 2017, Michael Neunert, Markus Giftthaler, Markus Stäuble, Diego Pardo,
Farbod Farshidian. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
 * Neither the name of ETH ZURICH nor the names of its contributors may be used
      to endorse or promote products derived from this software without specific
      prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL ETH ZURICH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ***************************************************************************************/


namespace ct{
namespace optcon{


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::startupRoutine()
{
	launchWorkerThreads();
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::shutdownRoutine()
{
	workersActive_ = false;
	workerTask_ = SHUTDOWN;
	workerWakeUpCondition_.notify_all();

#ifdef DEBUG_PRINT_MP
	std::cout<<"Shutting down workers"<<std::endl;
#endif // DEBUG_PRINT_MP

	for (size_t i=0; i<workerThreads_.size(); i++)
	{
		workerThreads_[i].join();
	}

#ifdef DEBUG_PRINT_MP
	std::cout<<"All workers shut down"<<std::endl;
#endif // DEBUG_PRINT_MP
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::threadWork(size_t threadId)
{
#ifdef DEBUG_PRINT_MP
	printString("[Thread "+std::to_string(threadId)+"]: launched");
#endif // DEBUG_PRINT_MP


	// local variables
	int workerTask_local = IDLE;
	size_t uniqueProcessID = 0;
	size_t iteration_local = this->iteration_;


	while(workersActive_)
	{
		workerTask_local = workerTask_.load();
		iteration_local = this->iteration_;

#ifdef DEBUG_PRINT_MP
		printString("[Thread " + std::to_string(threadId) + "]: previous procId: " + std::to_string(uniqueProcessID) +
				", current procId: " +std::to_string(generateUniqueProcessID(iteration_local, (int) workerTask_local)));
#endif


		/*!
		 * We want to put the worker to sleep if
		 * - the workerTask_ is IDLE
		 * - or we are finished but workerTask_ is not yet reset, thus the process ID is still the same
		 * */
		if (workerTask_local == IDLE || uniqueProcessID == generateUniqueProcessID(iteration_local, (int) workerTask_local))
		{
#ifdef DEBUG_PRINT_MP
			printString("[Thread " + std::to_string(threadId) + "]: going to sleep !");
#endif

			// sleep until the state is not IDLE any more and we have a different process ID than before
			std::unique_lock<std::mutex> waitLock(workerWakeUpMutex_);
			while(workerTask_ == IDLE ||  (uniqueProcessID == generateUniqueProcessID(this->iteration_, (int)workerTask_.load()))){
				workerWakeUpCondition_.wait(waitLock);
			}
			waitLock.unlock();

			workerTask_local = workerTask_.load();
			iteration_local = this->iteration_;

#ifdef DEBUG_PRINT_MP
			printString("[Thread " + std::to_string(threadId) + "]: woke up !");
#endif // DEBUG_PRINT_MP
		}

		if (!workersActive_)
			break;


		switch(workerTask_local)
		{
		case LINE_SEARCH:
		{
#ifdef DEBUG_PRINT_MP
			printString("[Thread " + std::to_string(threadId) + "]: now busy with LINE_SEARCH !");
#endif // DEBUG_PRINT_MP
			lineSearchWorker(threadId);
			uniqueProcessID = generateUniqueProcessID (iteration_local, LINE_SEARCH);
			break;
		}
		case ROLLOUT_SHOTS:
		{
#ifdef DEBUG_PRINT_MP
			printString("[Thread " + std::to_string(threadId) + "]: now doing shot rollouts !");
#endif // DEBUG_PRINT_MP
			rolloutShotWorker(threadId);
			uniqueProcessID = generateUniqueProcessID (iteration_local, ROLLOUT_SHOTS);
			break;
		}
		case LINEARIZE_DYNAMICS:
		{
#ifdef DEBUG_PRINT_MP
			printString("[Thread " + std::to_string(threadId) + "]: now doing linearization !");
#endif // DEBUG_PRINT_MP
			computeLinearizedDynamicsWorker(threadId);
			uniqueProcessID = generateUniqueProcessID (iteration_local, LINEARIZE_DYNAMICS);
			break;
		}
		case COMPUTE_COST:
		{
#ifdef DEBUG_PRINT_MP
			printString("[Thread " + std::to_string(threadId) + "]: now doing cost computation !");
#endif // DEBUG_PRINT_MP
			computeQuadraticCostsWorker(threadId);
			uniqueProcessID = generateUniqueProcessID (iteration_local, COMPUTE_COST);
			break;
		}
		case SHUTDOWN:
		{
#ifdef DEBUG_PRINT_MP
			printString("[Thread " + std::to_string(threadId) + "]: now shutting down !");
#endif // DEBUG_PRINT_MP
			return;
			break;
		}
		case IDLE:
		{
#ifdef DEBUG_PRINT_MP
			printString("[Thread " + std::to_string(threadId) + "]: is idle. Now going to sleep.");
#endif // DEBUG_PRINT_MP
			break;
		}
		default:
		{
			printString("[Thread " + std::to_string(threadId) + "]: WARNING: Worker has unknown task !");
			break;
		}
		}
	}
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::launchWorkerThreads()
{
	workersActive_ = true;
	workerTask_ = IDLE;

	for (int i=0; i < this->settings_.nThreads; i++)
	{
		workerThreads_.push_back(std::thread(&NLOCBackendMP::threadWork, this, i));
	}
}




template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::computeLinearizedDynamicsAroundTrajectory(size_t firstIndex, size_t lastIndex)
{
	/*!
	 * In special cases, this function may be called for a single index, e.g. for the unconstrained GNMS real-time iteration scheme.
	 * Then, don't wake up workers, but do single-threaded computation for that single index, and return.
	 */
	if(lastIndex == firstIndex)
	{
#ifdef DEBUG_PRINT_MP
		printString("[MP]: do single threaded linearization for single index "+ std::to_string(firstIndex) + ". Not waking up workers.");
#endif //DEBUG_PRINT_MP
		this->computeLinearizedDynamics(this->settings_.nThreads, firstIndex);
		return;
	}


	/*!
	 * In case of multiple points to be linearized, start multi-threading:
	 */
	Eigen::setNbThreads(1); // disable Eigen multi-threading
#ifdef DEBUG_PRINT_MP
	printString("[MP]: Restricting Eigen to " + std::to_string(Eigen::nbThreads()) + " threads.");
#endif //DEBUG_PRINT_MP

	kTaken_ = 0;
	kCompleted_ = 0;
	KMax_ = lastIndex;
	KMin_ = firstIndex;

#ifdef DEBUG_PRINT_MP
	printString("[MP]: Waking up workers to do linearization.");
#endif //DEBUG_PRINT_MP
	workerTask_ = LINEARIZE_DYNAMICS;
	workerWakeUpCondition_.notify_all();

#ifdef DEBUG_PRINT_MP
	printString("[MP]: Will sleep now until we have linearized dynamics.");
#endif //DEBUG_PRINT_MP

	std::unique_lock<std::mutex> waitLock(kCompletedMutex_);
	kCompletedCondition_.wait(waitLock, [this]{return kCompleted_.load() > KMax_ - KMin_;});
	waitLock.unlock();
	workerTask_ = IDLE;
#ifdef DEBUG_PRINT_MP
	printString("[MP]: Woke up again, should have linearized dynamics now.");
#endif //DEBUG_PRINT_MP


	Eigen::setNbThreads(this->settings_.nThreadsEigen); // restore Eigen multi-threading
#ifdef DEBUG_PRINT_MP
	printString("[MP]: Restoring " + std::to_string(Eigen::nbThreads()) + " Eigen threads.");
#endif //DEBUG_PRINT_MP
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::computeLinearizedDynamicsWorker(size_t threadId)
{
	while(true)
	{
		size_t k = kTaken_++;

		if (k > KMax_- KMin_)
		{
			//kCompleted_++;
			if (kCompleted_.load() > KMax_-KMin_)
				kCompletedCondition_.notify_all();
			return;
		}

#ifdef DEBUG_PRINT_MP
		if ((k+1)%100 == 0)
			printString("[Thread " + std::to_string(threadId) + "]: Linearizing for index k " + std::to_string ( KMax_-k ));
#endif

		this->computeLinearizedDynamics(threadId, KMax_-k); // linearize backwards

		kCompleted_++;
	}
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::computeQuadraticCostsAroundTrajectory(size_t firstIndex, size_t lastIndex)
{
	//! fill terminal cost
	this->initializeCostToGo();


	/*!
	 * In special cases, this function may be called for a single index, e.g. for the unconstrained GNMS real-time iteration scheme.
	 * Then, don't wake up workers, but do single-threaded computation for that single index, and return.
	 */
	if(lastIndex == firstIndex)
	{
#ifdef DEBUG_PRINT_MP
		printString("[MP]: do single threaded cost approximation for single index " + std::to_string(firstIndex) + ". Not waking up workers.");
#endif //DEBUG_PRINT_MP
		this->computeQuadraticCosts(this->settings_.nThreads, firstIndex);
		return;
	}


	/*!
	 * In case of multiple points to be linearized, start multi-threading:
	 */
	Eigen::setNbThreads(1); // disable Eigen multi-threading
#ifdef DEBUG_PRINT_MP
	printString("[MP]: Restricting Eigen to " + std::to_string(Eigen::nbThreads()) + " threads.");
#endif //DEBUG_PRINT_MP

	kTaken_ = 0;
	kCompleted_ = 0;
	KMax_ = lastIndex;
	KMin_ = firstIndex;


#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Waking up workers to do cost computation."<<std::endl;
#endif //DEBUG_PRINT_MP
	workerTask_ = COMPUTE_COST;
	workerWakeUpCondition_.notify_all();

#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Will sleep now until we have cost."<<std::endl;
#endif //DEBUG_PRINT_MP

	std::unique_lock<std::mutex> waitLock(kCompletedMutex_);
	kCompletedCondition_.wait(waitLock, [this]{return kCompleted_.load() > KMax_ - KMin_;});
	waitLock.unlock();
	workerTask_ = IDLE;
#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Woke up again, should have cost now."<<std::endl;
#endif //DEBUG_PRINT_MP

	Eigen::setNbThreads(this->settings_.nThreadsEigen); // restore Eigen multi-threading
#ifdef DEBUG_PRINT_MP
	printString("[MP]: Restoring " + std::to_string(Eigen::nbThreads()) + " Eigen threads.");
#endif //DEBUG_PRINT_MP
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::computeQuadraticCostsWorker(size_t threadId)
{
	while(true)
	{
		size_t k = kTaken_++;

		if (k > KMax_ - KMin_)
		{
			//kCompleted_++;
			if (kCompleted_.load() > KMax_ - KMin_)
				kCompletedCondition_.notify_all();
			return;
		}

#ifdef DEBUG_PRINT_MP
		if ((k+1)%100 == 0)
			printString("[Thread "+std::to_string(threadId)+"]: Quadratizing cost for index k "+std::to_string(KMax_ - k ));
#endif

		this->computeQuadraticCosts(threadId, KMax_ - k); // compute cost backwards

		kCompleted_++;
	}
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::computeLQProblemWorker(size_t threadId)
{
	while(true)
	{
		size_t k = kTaken_++;

		if (k > KMax_ - KMin_)
		{
			//kCompleted_++;
			if (kCompleted_.load() > KMax_ - KMin_)
				kCompletedCondition_.notify_all();
			return;
		}

#ifdef DEBUG_PRINT_MP
		if ((k+1)%100 == 0)
			printString("[Thread " + std::to_string(threadId) + "]: Building LQ problem for index k " + std::to_string(KMax_ - k));
#endif

		this->computeQuadraticCosts(threadId, KMax_-k); // compute cost backwards
		this->computeLinearizedDynamics(threadId, KMax_-k); // linearize backwards

		kCompleted_++;
	}
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::rolloutShots(size_t firstIndex, size_t lastIndex)
{
	/*!
	 * In special cases, this function may be called for a single index, e.g. for the unconstrained GNMS real-time iteration scheme.
	 * Then, don't wake up workers, but do single-threaded computation for that single index, and return.
	 */
	if(lastIndex == firstIndex)
	{
#ifdef DEBUG_PRINT_MP
		printString("[MP]: do single threaded shot rollout for single index " + std::to_string(firstIndex) + ". Not waking up workers.");
#endif //DEBUG_PRINT_MP

		this->rolloutSingleShot(this->settings_.nThreads, firstIndex, this->u_ff_, this->x_, this->x_, this->xShot_);

		this->computeSingleDefect(firstIndex, this->x_, this->xShot_, this->lqocProblem_->b_);
		return;
	}

	/*!
	 * In case of multiple points to be linearized, start multi-threading:
	 */
	Eigen::setNbThreads(1); // disable Eigen multi-threading
#ifdef DEBUG_PRINT_MP
	printString("[MP]: Restricting Eigen to " + std::to_string (Eigen::nbThreads())  + " threads.");
#endif //DEBUG_PRINT_MP


	kTaken_ = 0;
	kCompleted_ = 0;
	KMax_ = lastIndex;
	KMin_ = firstIndex;


#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Waking up workers to do shot rollouts."<<std::endl;
#endif //DEBUG_PRINT_MP
	workerTask_ = ROLLOUT_SHOTS;
	workerWakeUpCondition_.notify_all();

#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Will sleep now until we have rolled out all shots."<<std::endl;
#endif //DEBUG_PRINT_MP

	std::unique_lock<std::mutex> waitLock(kCompletedMutex_);
	kCompletedCondition_.wait(waitLock, [this]{return kCompleted_.load() > KMax_ - KMin_;});
	waitLock.unlock();
	workerTask_ = IDLE;
#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Woke up again, should have rolled out all shots now."<<std::endl;
#endif //DEBUG_PRINT_MP

	Eigen::setNbThreads(this->settings_.nThreadsEigen); // restore Eigen multi-threading
#ifdef DEBUG_PRINT_MP
	printString("[MP]: Restoring " + std::to_string(Eigen::nbThreads()) + " Eigen threads.");
#endif //DEBUG_PRINT_MP
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>:: rolloutShotWorker(size_t threadId)
{
	while(true)
	{
		size_t k = kTaken_++;

		if (k > KMax_ - KMin_)
		{
			if (kCompleted_.load() > KMax_ - KMin_)
				kCompletedCondition_.notify_all();
			return;
		}

		size_t kShot = (KMax_ - k);
		if(kShot % ((size_t)this->settings_.K_shot) == 0) //! only rollout when we're meeting the beginning of a shot
		{
#ifdef DEBUG_PRINT_MP
			if ((k+1)%100 == 0)
				printString("[Thread " + std::to_string(threadId) + "]: rolling out shot with index " + std::to_string(KMax_ - k));
#endif

			this->rolloutSingleShot(threadId, kShot, this->u_ff_, this->x_, this->x_, this->xShot_);

			this->computeSingleDefect(kShot, this->x_, this->xShot_, this->lqocProblem_->b_);

		}

		kCompleted_++;
	}
}


template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
SCALAR NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::performLineSearch()
{
	Eigen::setNbThreads(1); // disable Eigen multi-threading

	alphaProcessed_.clear();
	alphaTaken_ = 0;
	alphaBestFound_ = false;
	alphaExpBest_ = this->settings_.lineSearchSettings.maxIterations;
	alphaExpMax_ = this->settings_.lineSearchSettings.maxIterations;
	alphaProcessed_.resize(this->settings_.lineSearchSettings.maxIterations, 0);
	lowestCostPrevious_ = this->lowestCost_;

#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Waking up workers."<<std::endl;
#endif //DEBUG_PRINT_MP
	workerTask_ = LINE_SEARCH;
	workerWakeUpCondition_.notify_all();

#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Will sleep now until we have results."<<std::endl;
#endif //DEBUG_PRINT_MP
	std::unique_lock<std::mutex> waitLock(alphaBestFoundMutex_);
	alphaBestFoundCondition_.wait(waitLock, [this]{return alphaBestFound_.load();});
	waitLock.unlock();
	workerTask_ = IDLE;
#ifdef DEBUG_PRINT_MP
	std::cout<<"[MP]: Woke up again, should have results now."<<std::endl;
#endif //DEBUG_PRINT_MP

	double alphaBest = 0.0;
	if (alphaExpBest_ != alphaExpMax_)
	{
		alphaBest = this->settings_.lineSearchSettings.alpha_0 * std::pow(this->settings_.lineSearchSettings.n_alpha, alphaExpBest_);
	}

	Eigen::setNbThreads(this->settings_.nThreadsEigen); // restore Eigen multi-threading
#ifdef DEBUG_PRINT_MP
	printString("[MP]: Restoring " + std::to_string(Eigen::nbThreads()) + " Eigen threads.");
#endif //DEBUG_PRINT_MP

	if(this->settings_.printSummary)
	{
		this->lu_norm_ = this->template computeDiscreteArrayNorm<ct::core::ControlVectorArray<CONTROL_DIM, SCALAR>, 2>(this->u_ff_, this->u_ff_prev_);
		this->lx_norm_ = this->template computeDiscreteArrayNorm<ct::core::StateVectorArray<STATE_DIM, SCALAR>, 2>(this->x_, this->x_prev_);
	}else{
#ifdef MATLAB
		this->lu_norm_ = this->template computeDiscreteArrayNorm<ct::core::ControlVectorArray<CONTROL_DIM, SCALAR>, 2>(this->u_ff_, this->u_ff_prev_);
		this->lx_norm_ = this->template computeDiscreteArrayNorm<ct::core::StateVectorArray<STATE_DIM, SCALAR>, 2>(this->x_, this->x_prev_);
#endif
	}
	this->x_prev_ = this->x_;

	return alphaBest;

} // end linesearch



template <size_t STATE_DIM, size_t CONTROL_DIM, size_t P_DIM, size_t V_DIM, typename SCALAR>
void NLOCBackendMP<STATE_DIM, CONTROL_DIM, P_DIM, V_DIM, SCALAR>::lineSearchWorker(size_t threadId)
{
	while(true)
	{
		size_t alphaExp = alphaTaken_++;

#ifdef DEBUG_PRINT_MP
		printString("[Thread "+ std::to_string(threadId)+"]: Taking alpha index " + std::to_string(alphaExp));
#endif

		if (alphaExp >= alphaExpMax_ || alphaBestFound_)
		{
			return;
		}

		//! convert to real alpha
		double alpha = this->settings_.lineSearchSettings.alpha_0 * std::pow(this->settings_.lineSearchSettings.n_alpha, alphaExp);

		//! local variables
		SCALAR cost = std::numeric_limits<SCALAR>::max();
		SCALAR intermediateCost = std::numeric_limits<SCALAR>::max();
		SCALAR finalCost = std::numeric_limits<SCALAR>::max();
		SCALAR defectNorm = std::numeric_limits<SCALAR>::max();
		ct::core::StateVectorArray<STATE_DIM, SCALAR> x_search(this->K_+1);
		ct::core::StateVectorArray<STATE_DIM, SCALAR> x_shot_search(this->K_+1);
		ct::core::StateVectorArray<STATE_DIM, SCALAR> defects_recorded(this->K_+1, ct::core::StateVector<STATE_DIM, SCALAR>::Zero());
		ct::core::ControlVectorArray<CONTROL_DIM, SCALAR> u_recorded(this->K_);

		//! set init state
		x_search[0] = this->x_prev_[0];


		switch(this->settings_.nlocp_algorithm)
		{
		case NLOptConSettings::NLOCP_ALGORITHM::GNMS :
		{
			this->executeLineSearchMultipleShooting(threadId, alpha, this->lu_, this->lx_, x_search,
					x_shot_search, defects_recorded, u_recorded, intermediateCost, finalCost, defectNorm, &alphaBestFound_);
			break;
		}
		case NLOptConSettings::NLOCP_ALGORITHM::ILQR :
		{
			defectNorm = 0.0;
			this->executeLineSearchSingleShooting(threadId, alpha, x_search, u_recorded, intermediateCost, finalCost, &alphaBestFound_);
			break;
		}
		default :
			throw std::runtime_error("Algorithm type unknown in performLineSearch()!");
		}


		cost = intermediateCost + finalCost + this->settings_.meritFunctionRho * defectNorm;

		lineSearchResultMutex_.lock();
		if (cost < lowestCostPrevious_ && !std::isnan(cost))
		{
			// make sure we do not alter an existing result
			if (alphaBestFound_)
			{
				lineSearchResultMutex_.unlock();
				break;
			}

			if(this->settings_.lineSearchSettings.debugPrint){
				printString("[LineSearch, Thread " + std::to_string(threadId) + "]: Lower cost/merit found at alpha:"+ std::to_string(alpha));
				printString("[LineSearch]: Cost:\t" + std::to_string(intermediateCost + finalCost));
				printString("[LineSearch]: Defect:\t" + std::to_string(defectNorm));
				printString("[LineSearch]: Merit:\t" + std::to_string(cost));
			}

			alphaExpBest_ = alphaExp;
			this->intermediateCostBest_ = intermediateCost;
			this->finalCostBest_ = finalCost;
			this->d_norm_ = defectNorm;
			this->lowestCost_ = cost;
			this->x_.swap(x_search);
			this->xShot_.swap(x_shot_search);
			this->u_ff_.swap(u_recorded);
			this->lqocProblem_->b_.swap(defects_recorded);
		} else
		{
			if(this->settings_.lineSearchSettings.debugPrint){
				printString("[LineSearch, Thread " + std::to_string(threadId) + "]: NO lower cost/merit found at alpha:"+ std::to_string(alpha));
				printString("[LineSearch]: Cost:\t" + std::to_string(intermediateCost + finalCost));
				printString("[LineSearch]: Defect:\t" + std::to_string(defectNorm));
				printString("[LineSearch]: Merit:\t" + std::to_string(cost));
			}
		}

		alphaProcessed_[alphaExp] = 1;

		// we now check if all alphas prior to the best have been processed
		// this also covers the case that there is no better alpha
		bool allPreviousAlphasProcessed = true;
		for (size_t i=0; i<alphaExpBest_; i++)
		{
			if (alphaProcessed_[i] != 1)
			{
				allPreviousAlphasProcessed = false;
				break;
			}
		}
		if (allPreviousAlphasProcessed)
		{
			alphaBestFound_ = true;
			alphaBestFoundCondition_.notify_all();
		}

		lineSearchResultMutex_.unlock();
	}
}

} // namespace optcon
} // namespace ct

