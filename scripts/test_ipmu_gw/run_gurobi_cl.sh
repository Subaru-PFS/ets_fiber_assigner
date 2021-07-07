#!/bin/bash

function usage {

  cat <<EOM
Usage: $(basename "$0") [OPTION]...

  -m <MPS FILENAME>  : MPS FILE NAME
  -l <LOG NAME>      : LOG NAME for LOGFILE
  -g <MIPGap>        : MIPGap value        (default:0.01)
  -t <Threads>       : Number of Threads   (default:28)
  -p <Presolve>      : Presolve            (default:-1)
  -e <Method>        : Method              (default:-1)
  -d <DegenMoves>    : DegenMoves          (default:-1)
  -r <Heuristics>    : Heuristics          (default:0.05)
  -f <MIPFocus>      : MIPFocus            (default:0)
  -c <Cuts>          : Cuts                (default:-1)
  -n <Nseed>         : Number of seeds     (default:5)
  -h                 : Display help
EOM

  exit 2
}


mps_dir=../data/
log_dir=../logs/

# init parameters

mps=idl_ge_single_sampled_case1_fixed_net.mps
logname=idl_ge_single_sampled_case1_fixed_net_fiducial
mipgap=0.01
threads=28
presolve=-1
method=-1
degenmoves=-1
heuristics=0.05
mipfocus=0
cuts=-1
nseed=5

while getopts "m:l:g:t:p:e:d:r:f:c:n:" optKey; do
  case "$optKey" in
    m)
      mps=$OPTARG
      ;;
    l)
      logname=$OPTARG
      ;;
    g)
      mipgap=$OPTARG
      ;;
    t)
      threads=$OPTARG
      ;;
    p)
      presolve=$OPTARG
      ;;
    e)
      method=$OPTARG
      ;;
    d)
      degenmoves=$OPTARG
      ;;
    r)
      heuristics=$OPTARG
      ;;
    f)
      mipfocus=$OPTARG
      ;;
    c)
      cuts=$OPTARG
      ;;
    n)
      nseed=$OPTARG
      ;;	
    h|*)
      usage
      ;;
  esac
done

for seed in `seq -f %02g 1 $nseed`
do 
  arguments="MIPGap=$mipgap Threads=$threads Seed=$seed Presolve=$presolve Method=$method DegenMoves=$degenmoves Heuristics=$heuristics MIPFocus=$mipfocus Cuts=$cuts LogFile=$log_dir${logname}_seed$seed.log ${mps_dir}$mps"
  command="gurobi_cl $arguments " 
  echo $command
  $command
done
