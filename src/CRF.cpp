/*
 * Copyright (C) 2010 Minwoo Jeong (minwoo.j@gmail.com).
 * This file is part of the "TriCRF" distribution.
 * http://github.com/minwoo/TriCRF/
 * This software is provided under the terms of Modified BSD license: see LICENSE for the detail.
 */

/// max headers
#include "CRF.h"
#include "Evaluator.h"
#include "Utility.h"
#include "LBFGS.h"
/// standard headers
#include <cassert>
#include <cfloat>
#include <cmath>
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <fstream>

#define MAT3(I, X, Y)	((m_state_size * m_state_size * (I)) + (m_state_size * (X)) + Y)
#define MAT2(I, X)		((m_state_size * (I)) + X)

using namespace std;

namespace tricrf {

/** Constructor.
*/
CRF::CRF() {
	m_default_oid = 0;
}

CRF::CRF(Logger *logger) {
	setLogger(logger);
	logger->report(MAX_HEADER);
	logger->report(">> Conditional Random Fields << \n\n");
	m_default_oid = 0;
}

void CRF::clear() {
	m_Param.clear();
}

/** Save the model.
	@param filename file to be saved
	@return success or fail
*/
bool	CRF::saveModel(const std::string& filename) {
	/// Checking the error
	if (filename == "")
		return false;

	timer stop_watch;
	logger->report("[Model saving]\n");

	/// file stream
    ofstream f(filename.c_str());
    f.precision(20);
    if (!f)
        throw runtime_error("unable to open file to write");

    /// header
    f << "# MAX: A C++ Library for Structured Prediction" << endl;
	f << "# CRF Model file (text format)" << endl;
	f << "# Do not edit this file" << endl;
	f << "# " << endl << ":" << endl;

	bool ret = m_Param.save(f);
	f.close();
	logger->report("  saving time = \t%.3f\n\n", stop_watch.elapsed());

	return ret;
}

/** Load the model.
	@param filename file to be loaded
	@return success or fail
*/
bool CRF::loadModel(const std::string& filename) {
	/// Checking the error
	if (filename == "")
		return false;

	timer stop_watch;
	logger->report("[Model loading]\n");

	/// file stream
    ifstream f(filename.c_str());
    f.precision(20);
    if (!f)
        throw runtime_error("fail to open model file");

    /// header
	size_t count = 0;
    string line;
    getline(f, line);
    while (line.empty() || line[0] == '#') {
		if (count == 1) {
			vector<string> tok = tokenize(line);
			if (tok.size() < 2 || tok[1] != "CRF") {
				logger->report("|Error| Invalid model files ... \n");
				return false;
			}
		}
        getline(f, line);
		count++;
	}

	bool ret = m_Param.load(f);
	f.close();
	m_Param.print(logger);
	logger->report("  loading time = \t%.3f\n\n", stop_watch.elapsed());

	/// to be used in inference
	m_Param.makeStateIndex();
	m_state_size = m_Param.sizeStateVec();

	return ret;
}

/**	Read the data from file
*/
void CRF::readTrainData(const string& filename) {
	/// File stream
	string line;
	ifstream f(filename.c_str());
	if (!f)
		throw runtime_error("cannot open data file");

	// Make a state space Y
	while ( getline(f, line) ) {
		if ( !line.empty() ) {
			vector<string> tokens = tokenize(line);
			if (tokens.size() > 0) {
				size_t index = 0;
				string fstr(tokens[index]);
				vector<string> tok = tokenize(fstr, ":");
				float fval = 1.0;
				if (tok.size() > 1) {
					fval = atof(tok[1].c_str());	///< feature value
					fstr = tok[0];
				}

				m_Param.addNewState(fstr);	// outcome id

			}

		}
	}

	f.clear();
	f.seekg(0, ios::beg);


	/// initializing
	Sequence seq;
	m_TrainSet.clear();
	m_TrainSetCount.clear();

	size_t count = 0;
	string prev_label = "";
	timer stop_watch;
	logger->report("[Training data file loading]\n");

	/// To reduce the storage and computation
	map<vector<vector<string> >, size_t> train_data_map;
	vector<vector<string> > token_list;

	while (getline(f,line)) {
		vector<string> tokens = tokenize(line, " \t");
		if (line.empty() || tokens.size() <= 0) {	 ///< sequence break
			if (train_data_map.find(token_list) == train_data_map.end()) {
				m_TrainSet.append(seq);
				train_data_map.insert(make_pair(token_list, m_TrainSetCount.size()));
				m_TrainSetCount.push_back(1.0);
			} else {
				m_TrainSetCount[train_data_map[token_list]] += 1.0;
			}
			seq.clear();
			token_list.clear();
			prev_label = "";
			++count;
		} else {
			token_list.push_back(tokens);

			Event ev = packEvent(tokens);	///< observation features
			seq.push_back(ev);						///< append

			/// State transition features
			/// This can be extended to state-dependent observation features. (See Sutton and McCallum, 2006)
			if (prev_label != "") {
				size_t pid = m_Param.addNewObs("@" + prev_label);
				for (size_t i = 0; i < m_Param.sizeStateVec(); i++) {
					if (i == ev.label)
						m_Param.updateParam(ev.label, pid, ev.fval);
					else
						m_Param.updateParam(i, pid, 0.0);
				}

				//m_Param.updateParam(ev.label, pid, ev.fval);
			}
			prev_label = tokens[0];
		}	// else

	}	// while
	m_Param.endUpdate();

	logger->report("  # of data = \t\t%d\n", count);
	logger->report("  loading time = \t%.3f\n\n", stop_watch.elapsed());

	m_Param.makeStateIndex();
	m_state_size = m_Param.sizeStateVec();
}

/**	Read the data from file
*/
void CRF::readDevData(const string& filename) {
	/// File stream
	string line;
	ifstream f(filename.c_str());
	if (!f)
		throw runtime_error("cannot open data file");

	/// initializing
	Sequence seq;
	m_DevSet.clear();
	m_DevSetCount.clear();
	size_t count = 0;
	string prev_label = "";
	timer stop_watch;
	logger->report("[Dev data file loading]\n");

	/// To reduce the storage and computation
	map<vector<vector<string> >, size_t> dev_data_map;
	vector<vector<string> > token_list;

	while (getline(f,line)) {
		vector<string> tokens = tokenize(line, " \t");
		if (line.empty() || tokens.size() <= 0) {	 ///< sequence break
			if (dev_data_map.find(token_list) == dev_data_map.end()) {
				m_DevSet.append(seq);
				dev_data_map.insert(make_pair(token_list, m_DevSetCount.size()));
				m_DevSetCount.push_back(1.0);
			} else {
				m_DevSetCount[dev_data_map[token_list]] += 1.0;
			}
			seq.clear();
			token_list.clear();
			prev_label = "";
			++count;
		} else {
			token_list.push_back(tokens);

			Event ev = packEvent(tokens, &m_Param, true);	///< observation features
			seq.push_back(ev);						///< append

			prev_label = tokens[0];
		}	// else

	}	// while

	logger->report("  # of data = \t\t%d\n", count);
	logger->report("  loading time = \t%.3f\n\n", stop_watch.elapsed());
}


void CRF::calculateEdge() {
	double* theta = m_Param.getWeight();

	// state transition is independent of time t and training set
	m_M2.resize(m_state_size * m_state_size);
	fill(m_M2.begin(), m_M2.end(), 1.0);
	vector<StateParam>::iterator iter = m_Param.m_StateIndex.begin();
	for (; iter != m_Param.m_StateIndex.end(); ++iter) {
		m_M2[MAT2(iter->y1,iter->y2)] *= exp(theta[iter->fid] * iter->fval);
	}
}

/**	Calculate the factors.
	References
		1)	J. Lafferty et al., Conditional Random Fields: Probabilistic Models for Segmenting and Labeling Sequence Data, 2001, ICML.
		2) C. Sutton and A. McCallum, An Introduction to Conditional Random Fields for Relational Learning, 2006, Introduction to Statistical Relational Learning. Edited by Lise Getoor and Ben Taskar. MIT Press. 2006.
*/
void CRF::calculateFactors(Sequence &seq) {
	/// Initialization
	m_seq_size = seq.size() + 1;	///< sequence length
	double* theta = m_Param.getWeight();

	/// Factor matrix initialization
	//m_M.resize(m_seq_size * m_state_size * m_state_size);
	m_R.resize(m_seq_size * m_state_size);
	//fill(m_M.begin(), m_M.end(), 1.0);
	fill(m_R.begin(), m_R.end(), 1.0);

	// for efficient alpha-beta
	//m_IndexR.clear();
	//m_IndexR.resize(m_seq_size-1);

	/// Calculation
	double a = 0.0;
	for (size_t i = 0; i < m_seq_size-1; i++) {

		//vector<size_t> &pointer = m_IndexR[i];
		//map<size_t, size_t> temp;

		/// Observation factor
		/*
		vector<ObsParam> obs_param = m_Param.makeObsIndex(seq[i].obs);
		vector<ObsParam>::iterator iter = obs_param.begin();
		for(; iter != obs_param.end(); ++iter) {
			m_R[MAT2(i, iter->y)] *= exp(theta[iter->fid] * iter->fval);
			//if (temp.find(iter->y) == temp.end()) {
			//	temp.insert(make_pair(iter->y, 1));
			//	pointer.push_back(iter->y);
			//}
		}
		*/
		vector<pair<size_t, double> >::iterator iter = seq[i].obs.begin();
		for (; iter != seq[i].obs.end(); iter++) {
			vector<pair<size_t, size_t> >& param = m_Param.m_ParamIndex[iter->first];
			for (size_t j = 0; j < param.size(); ++j) {
				m_R[MAT2(i, param[j].first)] *= exp(theta[param[j].second] * iter->second);
			}
		}

		/* it is redundant
		if (i > 0) {
			vector<StateParam>::iterator iter = m_Param.m_StateIndex.begin();
			for (; iter != m_Param.m_StateIndex.end(); ++iter) {
				m_M[MAT3(i,iter->y1,iter->y2)] *= exp(theta[iter->fid] * iter->fval);
			}

		} ///< if
		*/

	}	///< for

}

/**	Forward Recursion.
	Computing and storing the alpha value.
*/
void CRF::forward() {
	m_Alpha.resize(m_seq_size * m_state_size);
	fill(m_Alpha.begin(), m_Alpha.end(), 0.0);

	scale.resize(m_seq_size);
	fill(scale.begin(), scale.end(), 1.0);

	long double sum = 0.0;
	for (size_t j = 0; j < m_state_size; j++) {
	//vector<size_t> &indexR = m_IndexR[0];
	//for (size_t y = 0; y < indexR.size(); y++) {
	//	size_t j = indexR[y];

		m_Alpha[MAT2(0, j)] += m_R[MAT2(0, j)] * 1.0; //m_M[MAT3(0, m_default_oid,j)];  // <start>->j transition is 1.0
		sum += m_Alpha[MAT2(0, j)];
	}
	for (size_t j = 0; j < m_state_size; j++)
		m_Alpha[MAT2(0, j)] /= sum;
	scale[0] = sum;

    for (size_t i = 1; i < m_seq_size-1; i++) {
		long double sum = 0.0;
        for (size_t j = 0; j < m_state_size; j++) {
		//vector<size_t> &indexR = m_IndexR[i];
		//for (size_t y = 0; y < indexR.size(); y++) {
		//	size_t j = indexR[y];
			size_t index = MAT2(i, j);
            //for (size_t k = 0; k < m_state_size; k++) {
			vector<size_t> &selectedState = m_Param.m_SelectedStateList1[j];
			for (size_t x = 0; x < selectedState.size(); x++) {
				size_t k = selectedState[x];
                m_Alpha[index] += m_Alpha[MAT2(i-1, k)] * m_R[index] * (m_M2[MAT2(k,j)] - 1.0);
           }
			m_Alpha[index] += m_R[index];
			sum += m_Alpha[index];
        }
		for (size_t j = 0; j < m_state_size; j++)
			m_Alpha[MAT2(i, j)] /= sum;
		scale[i] = sum;
    }

	for (size_t k = 0; k < m_state_size; k++) {
		m_Alpha[MAT2(m_seq_size-1, m_default_oid)] += m_Alpha[MAT2(m_seq_size-2, k)];
	}
	scale[m_seq_size-1] = m_Alpha[MAT2(m_seq_size-1, m_default_oid)];

}

/**	Backward Recursion.
	Computing and storing the beta value.
*/
void CRF::backward() {
	m_Beta.resize(m_seq_size * m_state_size);
	fill(m_Beta.begin(), m_Beta.end(), 0.0);

	scale2.resize(m_seq_size);
	fill(scale2.begin(), scale2.end(), 1.0);

	m_Beta[MAT2(m_seq_size-1, m_default_oid)] = 1.0; // / scale[m_seq_size-1];
	long double sum = 0.0;

	for (size_t k = 0; k < m_state_size; k++) {
	//vector<size_t> &indexR = m_IndexR[m_seq_size-2];
	//for (size_t y = 0; y < indexR.size(); y++) {
	///	size_t k = indexR[y];
		m_Beta[MAT2(m_seq_size-2, k)] += 1.0;
		sum += m_Beta[MAT2(m_seq_size-2, k)];
	}
	for (size_t k = 0; k < m_state_size; k++)
		m_Beta[MAT2(m_seq_size-2, k)] /= sum;
	scale2[m_seq_size-2] = sum;

    for (int i = m_seq_size-2; i >= 1; i--) {
		long double sum = 0.0;
		long double constant = 0.0;
		for (size_t k = 0; k < m_state_size; k++)
			constant += m_R[MAT2(i,k)] * m_Beta[MAT2(i, k)];

		for (size_t j = 0; j < m_state_size; j++) {
		//vector<size_t> &indexR = m_IndexR[i-1];
		//for (size_t y = 0; y < indexR.size(); y++) {
		//	size_t j = indexR[y];

			size_t index = MAT2(i-1, j);
			//for (size_t k = 0; k < m_state_size; k++) {
			vector<size_t> &selectedState = m_Param.m_SelectedStateList2[j];
			for (size_t x = 0; x < selectedState.size(); x++) {
				size_t k = selectedState[x];
                m_Beta[index] += m_R[MAT2(i,k)] * (m_M2[MAT2(j, k)] - 1.0) * m_Beta[MAT2(i, k)];
           }
			//m_Beta[MAT2(i-1, j)] /= scale[i-1];
			m_Beta[MAT2(i-1, j)] += constant;
			sum += m_Beta[index];
        } // for j
		for (size_t j = 0; j < m_state_size; j++)
			m_Beta[MAT2(i-1, j)] /= sum;
		scale2[i-1] = sum;

    } // for i
}

/**	Partition function (Z).
	@return normalizing constant
*/
long double CRF::getPartitionZ() {
    return m_Alpha[MAT2(m_seq_size-1, m_default_oid)];
}

/** Calculate prob. of y* sequence.
	@param seq			given data (y, x)
	@return probability
*/
long double CRF::calculateProb(Sequence& seq) {
	long double z = getPartitionZ();

    long double seq_prob = 1.0;
	long double tran = 1.0;
    size_t prev_y = m_default_oid;
    size_t y;
    for (size_t i=0; i < m_seq_size; i++) {
        if (i < m_seq_size-1) {
            y = seq[i].label;
			if (i > 0)
				tran = m_M2[MAT2(prev_y, y)];
			seq_prob *= m_R[MAT2(i,y)] * tran;
        } else {
            y = m_default_oid;
        }

        prev_y = y;

		seq_prob /= scale[i];

    }
    if (seq_prob == 0.0) {
        cerr << "seq_prob==0 ";
    }
    seq_prob = seq_prob / z;

    return seq_prob;
}

/** Viterbi search to find the best probable output sequence.
  Viterbi algorithm.
 @param prob		dummy probability vector
 @return outcome sequence
*/
vector<size_t> CRF::viterbiSearch(long double& prob) {
	/// Initialization
	vector<vector<size_t> > psi;
    vector<vector<long double> > delta;

	/// Search
    size_t i, j, k;
	size_t prev_max_j = m_default_oid;
	long double prev_maxj = -100000.0;

	// first node
	/*
    vector<size_t> tmp_psi_i;
    vector<long double> tmp_delta_i;
	long double maxj = -10000.0;
	size_t max_j = 0;
	for (j=0; j < m_state_size; j++) {
		long double max = m_R[MAT2(0, j)] * 1.0;
		tmp_delta_i.push_back(max / scale[0]);
		tmp_psi_i.push_back(m_default_oid);
		if (max > maxj) {
			maxj = max;
			max_j = j;
		}
	}
	delta.push_back(tmp_delta_i);
	psi.push_back(tmp_psi_i);

	size_t prev_max_j = max_j;
	long double prev_maxj = maxj;
	*/

	// 1 ~ T
    for (i=0; i < m_seq_size-1; i++) {
        vector<size_t> psi_i;
        vector<long double> delta_i;

		long double maxj = -10000.0;
		size_t max_j = 0;

        for (j=0; j < m_state_size; j++) {
            long double max = -10000.0;
            size_t max_k = 0;
            if (i == 0) {
                max = 1.0; //m_M[MAT3(i,m_default_oid,j)];
                max_k = m_default_oid;
            } else {
                for (k=0; k < m_state_size; k++) {
				//vector<size_t> &selectedState = m_Param.m_SelectedStateList1[j];
				//for (size_t x = 0; x < selectedState.size(); x++) {
				//	size_t k = selectedState[x];
					double val = delta[i-1][k] * m_M2[MAT2(k,j)];
                    if (val > max) {
                        max = val;
                        max_k = k;
                    }
                }

				/*
				// refer to Siddiqi and Moore, 2005
				if (max < prev_maxj) {
					max = prev_maxj;
					max_k = prev_max_j;
				}*/
            }

			max = max * m_R[MAT2(i, j)]; // / scale[i];

            delta_i.push_back(max);
            psi_i.push_back(max_k);

			if (max > maxj) {
				maxj = max;
				max_j = j;
			}
        } // for j

        delta.push_back(delta_i);
        psi.push_back(psi_i);

		prev_max_j = max_j;
		prev_maxj = maxj;

    } // for i

	// last path
	vector<size_t> psi_i(m_state_size, 0);
	vector<long double> delta_i(m_state_size, -10000.0);
	long double max = -10000.0;
	size_t max_k = 0;
	for (size_t k=0; k < m_state_size; k++) {
		double val = delta[m_seq_size-2][k];
		if (val > max) {
			max = val;
			max_k = k;
		}
	}
	//max /= scale[m_seq_size-1];
	delta_i[m_default_oid] = max;
	psi_i[m_default_oid] = max_k;
	delta.push_back(delta_i);
	psi.push_back(psi_i);

	/// Back-tracking
    vector<size_t> y_seq;
    size_t prev_y = m_default_oid;
    for (i = m_seq_size-1; i >= 1; i--) {
        size_t y = psi[i][prev_y];
        y_seq.push_back(y);
        prev_y = y;
    }
    reverse(y_seq.begin(), y_seq.end());
    prob = delta[m_seq_size-1][m_default_oid];

	return y_seq;
}

/** Training with LBFGS optimizer.
	@param max_iter	maximum number of iteration
	@param sigma	Gaussian prior variance
*/
bool CRF::estimateWithLBFGS(size_t max_iter, double sigma, bool L1, double eta) {
	LBFGS lbfgs;	///< LBFGS optimizer
	double* theta = m_Param.getWeight();
	double* gradient = m_Param.getGradient();

	Evaluator eval(m_Param);	///< Evaluator
	timer t;		///< timer

	/// Reporting
	m_Param.print(logger);
	logger->report("[Parameter estimation]\n");
	logger->report("  Method = \t\tLBFGS\n");
	logger->report("  Regularization = \t%s\n", (sigma ? (L1 ? "L1":"L2") : "none"));
	logger->report("  Penalty value = \t%.2f\n\n", sigma);
	logger->report("[Inference]\n");
	logger->report("  Method = \t\tStandard\n");
	logger->report("[Iterations]\n");
	logger->report("%4s %15s %8s %8s %8s %8s\n", "iter", "loglikelihood", "acc", "micro-f1", "macro-f1", "sec");

	double old_obj = 1e+37;
	int converge = 0;

	/// Training iteration
	m_Param.makeActiveIndex(0.0);

    for (size_t niter = 0 ;niter < (int)max_iter; ++niter) {

		/// Initializing local variables
        timer t2;	///< elapsed time for one iteration
		m_Param.initializeGradient();	///< gradient vector initialization
		eval.initialize();	///< evaluator intialization
		double time_for_inference = 0.0;
		double time_for_inference2 = 0.0;
		double time_for_factor = 0.0;
		double time_for_estimation = 0.0;
		double time_for_viterbi = 0.0;
		timer time_for_fb;


		calculateEdge();

		/// for each training set
        vector<Sequence>::iterator sit = m_TrainSet.begin();
		vector<double>::iterator count_it = m_TrainSetCount.begin();
        for (; sit != m_TrainSet.end(); ++sit, ++count_it) {
			Sequence::iterator it = sit->begin();
			double count = *count_it;
			vector<size_t> reference, hypothesis;

			/// Forward-Backward
			timer stop_watch;
			calculateFactors(*sit);
			time_for_factor += stop_watch.elapsed();
			stop_watch.restart();
			forward();
			time_for_inference += stop_watch.elapsed();
			stop_watch.restart();
			backward();
			time_for_inference2 += stop_watch.elapsed();
			long double zval = getPartitionZ();

			/// Evaluation
			stop_watch.restart();
            long double dummy_prob;
			vector<size_t> y_seq = viterbiSearch(dummy_prob);
			time_for_viterbi += stop_watch.elapsed();

			// calculate Y sequence
			long double y_seq_prob = calculateProb(*sit);
            if (!finite((double)y_seq_prob)) {
                cerr << "calculateProb:" << y_seq_prob << endl;
            }

			// for scaling factor
			vector<long double> prod_scale, prod_scale2;
			prod_scale.clear();
			prod_scale2.clear();
			long double prod = 1.0;
			for (int a = m_seq_size-1; a >= 0; a--) {
				prod *= scale[a];
				prod_scale.push_back(prod);
			}
			reverse(prod_scale.begin(), prod_scale.end());
			prod = 1.0;
			for (int a = m_seq_size-1; a >= 0; a--) {
				prod *= scale2[a];
				prod_scale2.push_back(prod);
			}
			reverse(prod_scale2.begin(), prod_scale2.end());

			stop_watch.restart();
			size_t prev_outcome = 0;
			size_t i = 0;
			for (; it != sit->end(); ++it, ++i) {	 /// for each node
				size_t outcome = it->label;
				reference.push_back(it->label);
				hypothesis.push_back(y_seq[i]);

				/// calculate the expectation
				/// E[~p] - E[p]

				long double scale_factor = prod_scale2[i] / prod_scale[i+1];
				long double scale_factor2 = prod_scale2[i] / prod_scale[i];

				/*
				vector<ObsParam> obs_param = m_Param.makeObsIndex(it->obs);
				vector<ObsParam>::iterator iter = obs_param.begin();
				for(; iter != obs_param.end(); ++iter) {
					long double prob =  m_Alpha[MAT2(i, iter->y)] * m_Beta[MAT2(i, iter->y)] / zval;
					prob *= scale_factor;
					//prob *= scale[i];
					gradient[iter->fid] += prob * iter->fval * count;
				}
				*/
				vector<pair<size_t, double> >::iterator iter = it->obs.begin();
				for (; iter != it->obs.end(); iter++) {
					vector<pair<size_t, size_t> >& param = m_Param.m_ParamIndex[iter->first];
					for (size_t j = 0; j < param.size(); ++j) {
						long double prob =  m_Alpha[MAT2(i, param[j].first)] * m_Beta[MAT2(i, param[j].first)] / zval;
						prob *= scale_factor;
						//prob *= scale[i];
						gradient[param[j].second] += prob * iter->second * count;
					}
				}

				if (i > 0) {

					vector<StateParam>::iterator iter = m_Param.m_StateIndex.begin();
					for (; iter != m_Param.m_StateIndex.end(); ++iter) {
						long double a_y;
						//if (i == 0) {
						//	if (iter->y1 == m_default_oid)
						//		a_y = 1.0;
						//	else
						//		a_y = 0.0;
						//} else {
							a_y = m_Alpha[MAT2(i-1, iter->y1)];
						//}
						long double b_y = m_Beta[MAT2(i, iter->y2)];
						long double m_yy = m_R[MAT2(i,iter->y2)] * m_M2[MAT2(iter->y1,iter->y2)];
						long double prob = a_y * b_y * m_yy / zval;
						prob *= scale_factor2;
						gradient[iter->fid] += prob * iter->fval * count;
					}
				}

				prev_outcome = outcome;

			} ///< for sequence
			time_for_estimation += stop_watch.elapsed();

			for (size_t c = 0; c < count; c++) {
				eval.addLikelihood(y_seq_prob);	/// loglikelihood
				eval.append(reference, hypothesis);	/// evaluation (accuracy and f1 score)
			}

		} ///< for m_TrainSet

		/*
		cout << "time for factor : " << time_for_factor << endl;
		cout << "time for alpha : " << time_for_inference << endl;
		cout << "time for beta : " << time_for_inference2 <<  endl;
		cout << "time for viterbi : " << time_for_viterbi << endl;
		//cout << "avg. state : "  << (double)xxx / (double)yyy << endl;
		cout << "time for estimation : " << time_for_estimation <<  endl;
		cout << "total : " << time_for_fb.elapsed() << endl;
		*/
		time_for_inference = 0.0;

		/////////////////////////////////////////////////////////////////////////////////
		/// Evaluation for dev set
		////////////////////////////////////////////////////////////////////////////////
		Evaluator dev_eval(m_Param);		///< Evaluator (sequence)
		dev_eval.initialize();	///< evaluator intialization
		timer stop_watch;
		double time_for_dev = 0.0;
		/// for each dev data
        sit = m_DevSet.begin();
		count_it = m_DevSetCount.begin();
        for (; sit != m_DevSet.end(); ++sit, ++count_it) {
			Sequence::iterator it = sit->begin();
			double count = *count_it;
			calculateFactors(*sit);
  			forward();
			long double zval = getPartitionZ();
            long double dummy_prob;
			vector<size_t> y_seq = viterbiSearch(dummy_prob);
			assert(y_seq.size() == sit->size());

			vector<size_t> reference, hypothesis;
			for (size_t i = 0; it != sit->end(); ++it, ++i) {	 /// for each node
				reference.push_back(it->label);
				hypothesis.push_back(y_seq[i]);
			}
			for (size_t c = 0; c < count; c++) {
				dev_eval.append(reference, hypothesis);
			}

		} ///< for each dev
		time_for_dev = stop_watch.elapsed();

		/// applying regularization
		size_t n_nonzero = 0;
		if (sigma) {
			if (L1) { /// L1 regularization
				for (size_t i = 0; i < m_Param.size(); ++i) {
					eval.subLoglikelihood(abs(theta[i] / sigma));
					if (theta[i] != 0.0)
						n_nonzero++;
				}
			}
			else {	/// L2 regularization
				n_nonzero = m_Param.size();
				for (size_t i = 0; i < m_Param.size(); ++i) {
					gradient[i] += theta[i] / sigma;
					eval.subLoglikelihood((theta[i] * theta[i]) / (2 * sigma));
				}
			}
        }

		double diff = (niter == 0 ? 1.0 : abs(old_obj - eval.getObjFunc()) / old_obj);
		if (diff < eta)
			converge++;
		else
			converge = 0;
		old_obj = eval.getObjFunc();
		if (converge == 3)
			break;

		/// LBFGS optimizer
		int ret = lbfgs.optimize(m_Param.size(), theta, eval.getObjFunc(), gradient, L1, sigma);
		if (ret < 0)
			return false;
		else if (ret == 0)
			return true;

		eval.calculateF1();
		if (m_DevSet.size() > 0) {
			dev_eval.calculateF1();
			logger->report("%4d %15E %8.3f %8.3f %8.3f %8.3f  |  %8.3f %8.3f %8.3f\n",
				niter, eval.getLoglikelihood(),
				eval.getAccuracy(), eval.getMicroF1()[2], eval.getMacroF1()[2], t2.elapsed(),
				dev_eval.getAccuracy(), dev_eval.getMicroF1()[2], dev_eval.getMacroF1()[2]);
		} else {
			logger->report("%4d %15E %8.3f %8.3f %8.3f %8.3f\n", niter, eval.getLoglikelihood(),
				eval.getAccuracy(), eval.getMicroF1()[2], eval.getMacroF1()[2], t2.elapsed());
		}

		m_Param.makeActiveIndex(0.0);

	} ///< for iter

	logger->report("  training time = \t%.3f\n\n", t.elapsed());

	return true;

}

/** Training with Pseudo-Likelihood
	@param max_iter	maximum number of iteration
	@param sigma	Gaussian prior variance
*/
bool CRF::estimateWithPL(size_t max_iter, double sigma, bool L1, double eta) {
	LBFGS lbfgs;	///< LBFGS optimizer
	double* theta = m_Param.getWeight();
	double* gradient = m_Param.getGradient();

	Evaluator eval(m_Param);	///< Evaluator
	timer t;		///< timer

	/// Reporting
	m_Param.print(logger);
	logger->report("[Parameter estimation]\n");
	logger->report("  Method = \t\tPL\n");
	logger->report("  Regularization = \t%s\n", (sigma ? (L1 ? "L1":"L2") : "none"));
	logger->report("  Penalty value = \t%.2f\n\n", sigma);
	logger->report("[Iterations]\n");
	logger->report("%4s %15s %8s %8s %8s %8s\n", "iter", "loglikelihood", "acc", "micro-f1", "macro-f1", "sec");

	double old_obj = 1e+37;
	int converge = 0;

	/// Training iteration
    for (size_t niter = 0 ;niter < (int)max_iter; ++niter) {

		/// Initializing local variables
        timer t2;	///< elapsed time for one iteration
		m_Param.initializeGradient();	///< gradient vector initialization
		eval.initialize();	///< evaluator intialization

		/// for each training set
        vector<Sequence>::iterator sit = m_TrainSet.begin();
		vector<double>::iterator count_it = m_TrainSetCount.begin();
        for (; sit != m_TrainSet.end(); ++sit, ++count_it) {
			Sequence::iterator it = sit->begin();
			double count = *count_it;
			size_t prev_outcome = m_default_oid;
			vector<size_t> reference, hypothesis;

			for (; it != sit->end(); ++it) {	 /// for each node
				/// evaluation
				size_t max_outcome = 0;
				vector<double> q(m_Param.sizeStateVec());
				fill(q.begin(), q.end(), 0.0);

				/// w * f (for all classes)
				/*
				vector<ObsParam> obs_param = m_Param.makeObsIndex(it->obs);
				vector<ObsParam>::iterator iter = obs_param.begin();
				for(; iter != obs_param.end(); ++iter) {
					q[iter->y] += theta[iter->fid] * iter->fval;
				}*/
				vector<pair<size_t, double> >::iterator iter = it->obs.begin();
				for (; iter != it->obs.end(); iter++) {
					vector<pair<size_t, size_t> >& param = m_Param.m_ParamIndex[iter->first];
					for (size_t j = 0; j < param.size(); ++j) {
						q[param[j].first] += theta[param[j].second] * iter->second;
					}
				}

				// y_{t-1} state
				for (vector<StateParam>::iterator iter = m_Param.m_StateIndex.begin();
						iter != m_Param.m_StateIndex.end(); ++iter) {
					if (iter->y1 == prev_outcome)
						q[iter->y2] += theta[iter->fid] * iter->fval;
				}

				/// normalize
				double sum = 0.0;
				double max = 0.0;
				for (size_t j=0; j < m_Param.sizeStateVec(); j++) {
					q[j] = exp(q[j]); // * y_prob[j];
					sum += q[j];
					if (q[j] > max) {
						max = q[j];
						max_outcome = j;
					}
				}
				for (size_t j=0; j < m_Param.sizeStateVec(); j++) {
					q[j] /= sum;
				}

				reference.push_back(it->label);
				hypothesis.push_back(max_outcome);

				/// calculate the expectation
				/// E[p] - E[~p]
				/*
				obs_param = m_Param.makeObsIndex(it->obs);
				iter = obs_param.begin();
				for(; iter != obs_param.end(); ++iter) {
					gradient[iter->fid] += q[iter->y] * iter->fval * count;
				}
				*/
				iter = it->obs.begin();
				for (; iter != it->obs.end(); iter++) {
					vector<pair<size_t, size_t> >& param = m_Param.m_ParamIndex[iter->first];
					for (size_t j = 0; j < param.size(); ++j) {
						gradient[param[j].second] += q[param[j].first] * iter->second * count;
					}
				}

				for (vector<StateParam>::iterator iter = m_Param.m_StateIndex.begin();
						iter != m_Param.m_StateIndex.end(); ++iter) {
					if (iter->y1 == prev_outcome)
						gradient[iter->fid] = q[iter->y2] * iter->fval * count;
				}

				/// loglikelihood
				for (size_t c = 0; c < count; c++) {
					eval.addLikelihood(q[it->label]);
				}

				prev_outcome = it->label;

			} ///< for sequence

			/// evaluation (accuracy and f1 score)
			for (size_t c = 0; c < count; c++) {
				eval.append(reference, hypothesis);
			}


		} ///< for m_TrainSet
		Evaluator dev_eval(m_Param);		///< Evaluator (sequence)
		dev_eval.initialize();	///< evaluator intialization

		/// applying regularization
		size_t n_nonzero = 0;
		if (sigma) {
			if (L1) { /// L1 regularization
				for (size_t i = 0; i < m_Param.size(); ++i) {
					eval.subLoglikelihood(abs(theta[i] / sigma));
					if (theta[i] != 0.0)
						n_nonzero++;
				}
			}
			else {	/// L2 regularization
				n_nonzero = m_Param.size();
				for (size_t i = 0; i < m_Param.size(); ++i) {
					gradient[i] += theta[i] / sigma;
					eval.subLoglikelihood((theta[i] * theta[i]) / (2 * sigma));
				}
			}
        }

		double diff = (niter == 0 ? 1.0 : abs(old_obj - eval.getObjFunc()) / old_obj);
		if (diff < eta)
			converge++;
		else
			converge = 0;
		old_obj = eval.getObjFunc();
		if (converge == 3)
			break;

		/// LBFGS optimizer
		int ret = lbfgs.optimize(m_Param.size(), theta, eval.getObjFunc(), gradient, L1, sigma);
		if (ret < 0)
			return false;
		else if (ret == 0)
			return true;

		eval.calculateF1();
		if (m_DevSet.size() > 0) {
			dev_eval.calculateF1();
			logger->report("%4d %15E %8.3f %8.3f %8.3f %8.3f  |  %8.3f %8.3f %8.3f\n",
				niter, eval.getLoglikelihood(),
				eval.getAccuracy(), eval.getMicroF1()[2], eval.getMacroF1()[2], t2.elapsed(),
				dev_eval.getAccuracy(), dev_eval.getMicroF1()[2], dev_eval.getMacroF1()[2]);
		} else {
			logger->report("%4d %15E %8.3f %8.3f %8.3f %8.3f\n", niter, eval.getLoglikelihood(),
				eval.getAccuracy(), eval.getMicroF1()[2], eval.getMacroF1()[2], t2.elapsed());
		}

	} ///< for iter

	logger->report("  training time = \t%.3f\n\n", t.elapsed());

	return true;

}

bool CRF::pretrain(size_t max_iter, double sigma, bool L1) {
		return estimateWithPL(max_iter, sigma, L1);
}

bool CRF::train(size_t max_iter, double sigma, bool L1) {
		return estimateWithLBFGS(max_iter, sigma, L1);
}

void CRF::evals(Sequence seq, std::vector<std::string> &output, std::vector<long double> &prob) {
	calculateEdge();
	calculateFactors(seq);
	forward();

	long double zval = getPartitionZ();
    long double dummy_prob;
	vector<size_t> y_seq = viterbiSearch(dummy_prob);
	assert(y_seq.size() == seq.size());

	Sequence::iterator it = seq.begin();
	size_t prev_y = m_default_oid;
	for (size_t i = 0; it != seq.end(); ++it, ++i) {	 /// for each node
		string y_seq_s = m_Param.getState().second[y_seq[i]];
		output.push_back(y_seq_s);
	}

	prob.clear();
	for (size_t i = 0; i < m_state_size; i++) {
		prob.push_back(m_Alpha[MAT2(m_seq_size-2, i)] / zval);
	}

}

void CRF::eval(Sequence seq, std::vector<std::string> &output, long double &prob) {
	calculateEdge();
	calculateFactors(seq);
	forward();

	long double zval = getPartitionZ();
    long double dummy_prob;
	vector<size_t> y_seq = viterbiSearch(dummy_prob);
	assert(y_seq.size() == seq.size());

	Sequence::iterator it = seq.begin();
	size_t prev_y = m_default_oid;
	for (size_t i = 0; it != seq.end(); ++it, ++i) {	 /// for each node
		string y_seq_s = m_Param.getState().second[y_seq[i]];
		output.push_back(y_seq_s);
	}

	for (size_t i = 0; i < m_seq_size - 1; i++)
		dummy_prob /= scale[i];
		//zval *= scale[i];
	prob =  dummy_prob / zval;

}

void CRF::eval(Sequence seq, std::vector<std::string> &output, std::vector<long double> &prob) {
	calculateEdge();
	calculateFactors(seq);
	forward();
	backward();

	output.clear();
	prob.clear();

	long double zval = getPartitionZ();
    long double dummy_prob;
	vector<size_t> y_seq = viterbiSearch(dummy_prob);
	assert(y_seq.size() == seq.size());


	// scale factor
	vector<long double> prod_scale, prod_scale2;
	prod_scale.clear();
	prod_scale2.clear();
	long double prod = 1.0;
	for (int a = m_seq_size-1; a >= 0; a--) {
		prod *= scale[a];
		prod_scale.push_back(prod);
	}
	reverse(prod_scale.begin(), prod_scale.end());
	prod = 1.0;
	for (int a = m_seq_size-1; a >= 0; a--) {
		prod *= scale2[a];
		prod_scale2.push_back(prod);
	}
	reverse(prod_scale2.begin(), prod_scale2.end());


	Sequence::iterator it = seq.begin();
	size_t prev_y = m_default_oid;
	for (size_t i = 0; it != seq.end(); ++it, ++i) {	 /// for each node
		string y_seq_s = m_Param.getState().second[y_seq[i]];
		output.push_back(y_seq_s);

		size_t outcome = it->label;
		long double scale_factor = prod_scale2[i] / prod_scale[i+1];

		long double p =  m_Alpha[MAT2(i, y_seq[i])] * m_Beta[MAT2(i, y_seq[i])] / zval;
		p *= scale_factor;
		prob.push_back(p);

	}

}

bool CRF::test(const std::string& filename, const std::string& outputfile, bool confidence) {
	/// File stream
	string line;
	ifstream f(filename.c_str());
	if (!f)
		throw runtime_error("cannot open data file");

	/// output
	ofstream out;
	vector<string> state_vec;
	if (outputfile != "") {
		out.open(outputfile.c_str());
		out.precision(20);
		state_vec = m_Param.getState().second;
	}

	/// initializing
	size_t count = 0;
	Sequence seq;
	logger->report("[Testing begins ...]\n");
	timer stop_watch;
	Evaluator test_eval(m_Param); ///< Evaluator
	test_eval.initialize(); ///< Evaluator intialization

	calculateEdge();

	/// reading the text
	while (getline(f,line)) {
		if (line.empty()) {
			/// test
			calculateFactors(seq);
  			forward();

			long double zval = getPartitionZ();
            long double dummy_prob;
			vector<size_t> y_seq = viterbiSearch(dummy_prob);
			assert(y_seq.size() == seq.size());

			vector<string> reference, hypothesis;

			Sequence::iterator it = seq.begin();
			size_t prev_y = m_default_oid;

			for (size_t i = 0; it != seq.end(); ++it, ++i) {	 /// for each node

				string outcome_s;
				if (m_Param.sizeStateVec() <= it->label)
					outcome_s = "!OUT_OF_CLASS!";
				else
					outcome_s = m_Param.getState().second[it->label];
				string y_seq_s = m_Param.getState().second[y_seq[i]];
				reference.push_back(outcome_s);
				hypothesis.push_back(y_seq_s);

				if (outputfile != "") {
					out << state_vec[y_seq[i]];
					if (confidence) {
						double norm = 0.0;
						for (size_t j = 0; j < m_state_size; j++) {
							if (i > 0)
								norm += m_R[MAT2(i, j)] * m_M2[MAT2(prev_y, j)];
							else
								norm += m_R[MAT2(i, j)];
						}
						double prob;
						if (i > 0)
							prob = m_R[MAT2(i,y_seq[i])] * m_M2[MAT2(prev_y,y_seq[i])] / norm;
						else
							prob = m_R[MAT2(i,y_seq[i])] / norm;
						out << " " << prob;
						prev_y = y_seq[i];
					}
					out << endl;
				}
			}
			if (outputfile != "")
				out << endl;


			test_eval.append(m_Param, reference, hypothesis);
			seq.clear();
			++count;
		} else {
			vector<string> tokens = tokenize(line);
			Event ev = packEvent(tokens, &m_Param, true);	///< observation features
			seq.push_back(ev);						///< append

		}	///< else
	}	///< while

	test_eval.calculateF1();
	logger->report("  # of data = \t\t%d\n", count);
	logger->report("  testing time = \t%.3f\n\n", stop_watch.elapsed());
	logger->report("  Acc = \t\t%8.3f\n", test_eval.getAccuracy());
	logger->report("  MicroF1 = \t\t%8.3f\n", test_eval.getMicroF1()[2]);
	//logger->report("  MacroF1 = \t\t%8.3f\n", test_eval.getMacroF1()[2]);
	test_eval.Print(logger);
}


}	///< namespace tricrf

