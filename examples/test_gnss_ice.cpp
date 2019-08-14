/*
 * @file test_gnss_ice.cpp
 * @brief Iterative GPS Range/Phase Estimator with collected data
 * @author Ryan Watson
 */

// GTSAM related includes.
#include <gtsam/slam/dataset.h>
#include <gtsam/nonlinear/ISAM2.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/gnssNavigation/GnssData.h>
#include <gtsam/gnssNavigation/GnssTools.h>
#include <gtsam/gnssNavigation/nonBiasStates.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/gnssNavigation/GNSSMultiModalFactor.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>


// LibCluster
#include <libcluster/merge.h>
#include <libcluster/probutils.h>
#include <libcluster/libcluster.h>
#include <libcluster/distributions.h>

// BOOST
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/serialization/export.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

// GPSTK
#include <gpstk/ConfDataReader.hpp>

// STD
#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>

using namespace std;
using namespace gtsam;
using namespace gpstk;
using namespace boost;
using namespace merge;
using namespace std::chrono;
using namespace libcluster;
using namespace distributions;
namespace NM = gtsam::noiseModel;
namespace po = boost::program_options;
typedef noiseModel::Diagonal diagNoise;

// Intel Threading Building Block
#ifdef GTSAM_USE_TBB
  #include <tbb/tbb.h>
  #undef max // TBB seems to include windows.h and we don't want these macros
  #undef min
#endif

using symbol_shorthand::X; // nonBiasStates ( dx, dy, dz, trop, cb )
using symbol_shorthand::G;   // bias states ( Phase Biases )

int main(int argc, char* argv[])
{
        // define std out print color
        vector<int> prn_vec;
        vector<int> factor_count_vec;
        vector<rnxData> data;
        // vector<faultyRnxData> data;
        const string red("\033[0;31m");
        const string green("\033[0;32m");
        string confFile, gnssFile, station;
        double xn, yn, zn, range, phase, rho, gnssTime;
        int startKey(0), currKey, startEpoch(0), svn, numBatch(0), state_count(0);
        int nThreads(-1), phase_break, break_count(0), nextKey, factor_count(-1), res_count(-1);
        bool printECEF, printENU, printAmb, first_ob(true);
        Eigen::MatrixXd residuals;
        vector<mixtureComponents> globalMixtureModel;

        cout.precision(12);

        po::options_description desc("Available options");
        desc.add_options()
                ("help,h", "Print help message")
                ("confFile,c", po::value<string>(&confFile)->default_value(""),
                "Input config file" );

        po::variables_map vm;
        po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
        po::notify(vm);

        ConfDataReader confReader;
        confReader.open(confFile);

        if (confFile.empty() ) {
                cout << red << "\n\n Currently, you need to provide a conf file \n"
                     << "\n\n"  << green << desc << endl;
        }

        while ( (station = confReader.getEachSection()) != "" )
        {
                xn = confReader.fetchListValueAsDouble("nominalECEF",station);
                yn = confReader.fetchListValueAsDouble("nominalECEF",station);
                zn = confReader.fetchListValueAsDouble("nominalECEF",station);
                printENU = confReader.getValueAsBoolean("printENU", station);
                printAmb = confReader.getValueAsBoolean("printAmb", station);
                printECEF = confReader.getValueAsBoolean("printECEF", station);
                gnssFile = confReader("dataFile", station);
        }

        Point3 nomXYZ(xn, yn, zn);
        Point3 prop_xyz = nomXYZ;

        // try { data = readGNSSFaulty(gnssFile, 50.0, 10.0, 0.3); }
        // catch(std::exception& e)
        // {
        //         cout << red << "\n\n Cannot read GNSS data file " << endl;
        //         exit(1);
        // }


        try {data = readGNSS_SingleFreq(gnssFile); }
        catch(std::exception& e)
        {
                cout << red << "\n\n Cannot read GNSS data file " << endl;
                exit(1);
        }


        #ifdef GTSAM_USE_TBB
        std::auto_ptr<tbb::task_scheduler_init> init;
        if(nThreads > 0) {
                init.reset(new tbb::task_scheduler_init(nThreads));
        }
        else
                cout << green << " \n\n Using threads for all processors" << endl;
        #else
        if(nThreads > 0) {
                cout << red <<" \n\n GTSAM is not compiled with TBB, so threading is"
                     << " disabled and the --threads option cannot be used."
                     << endl;
                exit(1);
        }
        #endif

        ISAM2DoglegParams doglegParams;
        ISAM2Params parameters;
        parameters.relinearizeThreshold = 0.01;
        parameters.relinearizeSkip = 10;
        ISAM2 isam(parameters);

        double output_time = 0.0;
        double rangeWeight = pow(2.5,2);
        double phaseWeight = pow(0.25,2);

        ifstream file(gnssFile.c_str());
        string value;

        nonBiasStates prior_nonBias = (gtsam::Vector(5) << 0.0, 0.0, 0.0, 0.0, 0.0).finished();

        phaseBias bias_state(Z_1x1);
        gnssStateVector phase_arc(Z_34x1);
        gnssStateVector bias_counter(Z_34x1);
        for (int i=1; i<34; i++) {bias_counter(i) = bias_counter(i-1) + 10000; }

        nonBiasStates initEst(Z_5x1);
        nonBiasStates between_nonBias_State(Z_5x1);

        Values initial_values;
        Values result;

        noiseModel::Diagonal::shared_ptr nonBias_InitNoise = noiseModel::Diagonal::Variances((gtsam::Vector(5) << 0.2, 0.2, 0.2, 3e6, 1e-1).finished());

        noiseModel::Diagonal::shared_ptr nonBias_ProcessNoise = noiseModel::Diagonal::Variances((gtsam::Vector(5) << 0.5, 0.5, 0.5, 1e3, 1e-3).finished());

        noiseModel::Diagonal::shared_ptr initNoise = noiseModel::Diagonal::Variances((gtsam::Vector(1) << 3e6).finished());

        // noiseModel::Diagonal::shared_ptr nonBias_InitNoise = noiseModel::Diagonal::Variances((gtsam::Vector(5) << 10.0, 10.0, 10.0, 3e8, 1e-1).finished());
        //
        // noiseModel::Diagonal::shared_ptr nonBias_ProcessNoise = noiseModel::Diagonal::Variances((gtsam::Vector(5) << 0.1, 0.1, 0.1, 3e6, 3e-5).finished());
        //
        // noiseModel::Diagonal::shared_ptr initNoise = noiseModel::Diagonal::Variances((gtsam::Vector(1) << 100).finished());

        NonlinearFactorGraph *graph = new NonlinearFactorGraph();

        residuals.setZero(2500,2);

        // init. mixture model.
        // Init this from file later
        Eigen::RowVectorXd m(2);
        m << 0.0, 0.0;

        // Add comp 1.
        Eigen::MatrixXd c(2,2);
        c<< rangeWeight, 0.0, 0.0, phaseWeight;
        globalMixtureModel.push_back(boost::make_tuple(0, 0, 0.0, m, c));

        //Add comp 2.
        c << rangeWeight*10.0, 0.0, 0.0, phaseWeight*10.0;
        // globalMixtureModel.push_back(boost::make_tuple(0, 0, 0.0, m, c));

        int lastStep = get<0>(data.back());

        for(unsigned int i = startEpoch; i < data.size(); i++ ) {

                double gnssTime = get<0>(data[i]);
                int currKey = get<1>(data[i]);
                if (first_ob) {
                        first_ob=false;
                        startKey = currKey;
                        graph->add(PriorFactor<nonBiasStates>(X(currKey), initEst,  nonBias_InitNoise));
                        ++factor_count;
                        initial_values.insert(X(currKey), initEst);
                }
                int nextKey = get<1>(data[i+1]);
                int svn = get<2>(data[i]);
                Point3 satXYZ = get<3>(data[i]);
                double rho = get<4>(data[i]);
                double range = get<5>(data[i]);
                double phase = get<6>(data[i]);
                double phase_break = get<7>(data[i]);

                gtsam::Vector2 obs;
                obs << range-rho, phase-rho;

                if ( currKey == startKey || phase_arc[svn]!=phase_break)
                {
                        bias_state[0] = phase-range;
                        if (currKey > startKey) { bias_counter[svn] = bias_counter[svn] +1; }
                        initial_values.insert(G(bias_counter[svn]), bias_state);
                        graph->add(boost::make_shared<PriorFactor<phaseBias> >(G(bias_counter[svn]), bias_state,  initNoise));
                        phase_arc[svn] = phase_break;
                        ++factor_count;
                }

                graph->add(boost::make_shared<GNSSMultiModalFactor>(X(currKey), G(bias_counter[svn]), obs, satXYZ, nomXYZ, globalMixtureModel));
                // graph->add(boost::make_shared<GNSSMultiModalFactor>(X(currKey), G(bias_counter[svn]), obs, satXYZ, prop_xyz, globalMixtureModel));

                prn_vec.push_back(svn);
                factor_count_vec.push_back(++factor_count);

                if (currKey != nextKey && nextKey != 0) {
                        if (currKey > startKey ) {
                                if ( lastStep == nextKey ) { break; }
                                double scale = (get<0>(data[i+1])-get<0>(data[i]))*10.0;
                                nonBias_ProcessNoise = noiseModel::Diagonal::Variances((gtsam::Vector(5) << 0.5*scale, 0.5*scale, 0.5*scale, 1e3*scale, 1e-3*scale).finished());

                                graph->add(boost::make_shared<BetweenFactor<nonBiasStates> >(X(currKey), X(currKey-1), initEst, nonBias_ProcessNoise));
                                ++factor_count;

                                if (state_count < 500)
                                {
                                        ++state_count;
                                        graph->add(PriorFactor<nonBiasStates>(X(currKey), initEst,  nonBias_InitNoise));

                                        ++factor_count;
                                }

                        }
                        isam.update(*graph, initial_values);
                        isam.update();
                        result = isam.calculateEstimate();

                        prior_nonBias = result.at<nonBiasStates>(X(currKey));
                        Point3 delta_xyz = (gtsam::Vector(3) << prior_nonBias.x(), prior_nonBias.y(), prior_nonBias.z()).finished();
                        prop_xyz = nomXYZ - delta_xyz;

                        if (printECEF) {
                                cout << "xyz " << gnssTime << " " << prop_xyz.x() << " " << prop_xyz.y() << " " << prop_xyz.z() << endl;
                        }

                        if (printENU) {
                                Point3 enu = xyz2enu(prop_xyz, nomXYZ);
                                cout << "enu " << gnssTime << " " << enu.x() << " " << enu.y() << " " << enu.z() << endl;
                        }

                        if (printAmb) {
                                cout << "gps " << " " << gnssTime << " ";
                                for (int k=0; k<prn_vec.size(); k++) {
                                        cout << result.at<phaseBias>(G(bias_counter[prn_vec[k]])) << " ";
                                }
                                cout << endl;
                        }

                        output_time = output_time +1;

                        // Get residuals from graph
                        for (int i = 0; i<factor_count_vec.size(); i++) {
                                ++res_count;
                                if (res_count > 2499 )
                                {
                                        residuals.conservativeResize(residuals.rows()+1, residuals.cols());

                                        residuals.row(residuals.rows()-1) = graph->at(factor_count_vec[i])->residual(result).transpose();
                                }
                                else
                                {
                                        residuals.block(res_count,0,1,2) << graph->at(factor_count_vec[i])->residual(result).transpose();
                                }
                        }
                        factor_count_vec.clear();
                        factor_count = -1;


                        string res_str = "batch_" + to_string(numBatch) + ".residuals";
                        ofstream res_os(res_str);
                        if (res_count >= 2500)
                        {
                                ++numBatch;
                                for (unsigned int i=0; i<residuals.rows()-1; i++)
                                {
                                        res_os << residuals.block(i,0,1,2) << endl;
                                }

                                StickBreak weights;
                                vector<GaussWish> clusters;
                                Eigen::MatrixXd qZ;

                                std::cout << "Trying to Cluster" << endl;
                                learnVDP(residuals, qZ, weights, clusters);

                                std::cout << "Trying to Merge" << endl;
                                globalMixtureModel = mergeMixtureModel(residuals, qZ, globalMixtureModel, clusters, weights, 0.1, 15);

                                cout << "\n\n\n\n\n\n" << endl;
                                cout << "----------------- Merged MODEL ----------------" << endl;
                                for (int i=0; i<globalMixtureModel.size(); i++)
                                {
                                        mixtureComponents mc = globalMixtureModel[i];
                                        auto cov = mc.get<4>();
                                        cout << mc.get<0>() << " " << mc.get<1>() << " "  <<  mc.get<2>() << "    " << mc.get<3>() <<"     "<< cov(0,0) << " " << cov(0,1) << " " << cov(1,1) <<"     "<<"\n\n" << endl;
                                }

                                residuals.setZero(2500,2);
                                res_count = -1;
                        }

                        graph->resize(0);
                        initial_values.clear();
                        prn_vec.clear();
                        initial_values.insert(X(nextKey), prior_nonBias);
                        // initial_values.insert(X(nextKey), initEst);
                        // graph->add(PriorFactor<nonBiasStates>(X(currKey), prior_nonBias, nonBias_ProcessNoise));
                        // ++factor_count;
                }

        }
        // isam.saveGraph("gnss.tree");

        cout << "\n\n\n\n\n\n" << endl;
        cout << "----------------- Final Mixture MODEL ----------------" << endl;
        for (int i=0; i<globalMixtureModel.size(); i++)
        {
                mixtureComponents mc = globalMixtureModel[i];
                auto cov = mc.get<4>();
                cout << mc.get<0>() << " " << mc.get<1>() << " "  <<  mc.get<2>() << "    " << mc.get<3>() <<"     "<< cov(0,0) << " " << cov(0,1) << " " << cov(1,1) <<"     "<<"\n\n" << endl;
        }

        return 0;
}
