#!/bin/bash
 
# The shell script which interprets this job script.
# Any option for qsub can be described like this,
# with #PBS
#PBS -S /bin/bash
 

# Merge STDERR to STDOUT
#PBS -j oe
 
# Path of the redirect of STDOUT by the batch job.
#PBS -o check_various_mps_sampled_case5_test_result

# No mail will be sent for the job.
#PBS -m p

#PBS -l nodes=1:ppn=28
#PBS -l walltime=72:00:00

# Name of queue;
# run qmgr -c "p s" to get the list of available queues.
#PBS -q mini

# Name of the job
# - within 15 characters length,
# - without white space characters,
# - and with the first character alphabetic.
# It is shown in the list which "qstat" generates
#PBS -N sampled_case5_test

cd /work/kiyoyabe/pfs/survey_simulations/scripts/check_netflow_processing_time_201905/scripts
source /home/kiyoyabe/gurobi/gurobi_811.env


# example (changing MIPFocus from 0 to 3)
rm -f ../logs/idl_ge_single_sampled_case5_fixed_net_test_mipfocus?_seed??.log

./run_gurobi_cl.sh -m idl_ge_single_unsampled_case5_fixed_net.mps -l idl_ge_single_sampled_case5_fixed_net_test_mipfocus0 -g 0.01 -t 28 -p 2 -e 4 -d 0 -r 0.8 -f 0 -c -1 -n 5
./run_gurobi_cl.sh -m idl_ge_single_unsampled_case5_fixed_net.mps -l idl_ge_single_sampled_case5_fixed_net_test_mipfocus1 -g 0.01 -t 28 -p 2 -e 4 -d 0 -r 0.8 -f 1 -c -1 -n 5
./run_gurobi_cl.sh -m idl_ge_single_unsampled_case5_fixed_net.mps -l idl_ge_single_sampled_case5_fixed_net_test_mipfocus2 -g 0.01 -t 28 -p 2 -e 4 -d 0 -r 0.8 -f 2 -c -1 -n 5
./run_gurobi_cl.sh -m idl_ge_single_unsampled_case5_fixed_net.mps -l idl_ge_single_sampled_case5_fixed_net_test_mipfocus3 -g 0.01 -t 28 -p 2 -e 4 -d 0 -r 0.8 -f 3 -c -1 -n 5

# for options for run_gurobi_cl.sh, run ./run_gurobi_cl.sh -h
