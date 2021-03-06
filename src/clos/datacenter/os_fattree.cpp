// -*- c-basic-offset: 4; tab-width: 8; indent-tabs-mode: t -*-
#include "os_fattree.h"
#include <vector>
#include "string.h"
#include <sstream>
#include <strstream>
#include <iostream>
#include "main.h"
#include "queue.h"
#include "switch.h"
#include "compositequeue.h"
#include "prioqueue.h"
#include "queue_lossless.h"
#include "queue_lossless_input.h"
#include "queue_lossless_output.h"
#include "ecnqueue.h"

extern uint32_t RTT_rack;
extern uint32_t RTT_net;
extern uint32_t SPEED;
extern ofstream fct_util_out;

string ntoa(double n);
string itoa(uint64_t n);

//extern int N;

OverSubscribedFatTree::OverSubscribedFatTree(int k, int racksz, mem_b queuesize, Logfile* lg, 
				 EventList* ev,FirstFit * fit,queue_type q){

    _queuesize = queuesize;
    logfile = lg;
    eventlist = ev;
    ff = fit;
    qt = q;
    failed_links = 0;

    K = k;
    Nhpr = racksz;

    assert(Nhpr >= K/2); // No undersubscribe

    Nlp = k * k / 2;
    Nup = k * (k - racksz);

    Nc = Nup / 2;

    _no_of_nodes = Nhpr * k * k/2;
    Nh = _no_of_nodes;

    cerr << "Built fat tree with k=" << k << ", nhost=" << Nh << ", os_ratio=" << Nhpr << "/" << (Nup / k) << endl;
 
    set_params(_no_of_nodes);

    init_network();
}

void OverSubscribedFatTree::set_params(int no_of_nodes) {

    switches_lp.resize(Nlp,NULL); // lower pod (ToRs)
    switches_up.resize(Nup,NULL); // upper pod (agg)
    switches_c.resize(Nc,NULL); // core

    pipes_nc_nup.resize(Nc, vector<Pipe*>(Nup));
    pipes_nup_nlp.resize(Nup, vector<Pipe*>(Nlp));
    pipes_nlp_ns.resize(Nlp, vector<Pipe*>(Nh));

    queues_nc_nup.resize(Nc, vector<Queue*>(Nup));
    queues_nup_nlp.resize(Nup, vector<Queue*>(Nlp));
    queues_nlp_ns.resize(Nlp, vector<Queue*>(Nh));

    pipes_nup_nc.resize(Nup, vector<Pipe*>(Nc));
    pipes_nlp_nup.resize(Nlp, vector<Pipe*>(Nup));
    pipes_ns_nlp.resize(Nh, vector<Pipe*>(Nlp));

    queues_nup_nc.resize(Nup, vector<Queue*>(Nc));
    queues_nlp_nup.resize(Nlp, vector<Queue*>(Nup));
    queues_ns_nlp.resize(Nh, vector<Queue*>(Nlp));
}

Queue* OverSubscribedFatTree::alloc_src_queue(QueueLogger* queueLogger){
    return  new PriorityQueue(speedFromMbps((uint64_t)SPEED), memFromPkt(FEEDER_BUFFER), *eventlist, queueLogger);
}

Queue* OverSubscribedFatTree::alloc_queue(QueueLogger* queueLogger, mem_b queuesize){
    return alloc_queue(queueLogger, SPEED, queuesize);
}

Queue* OverSubscribedFatTree::alloc_queue(QueueLogger* queueLogger, uint64_t speed, mem_b queuesize){
    if (qt==RANDOM)
	return new RandomQueue(speedFromMbps(speed), memFromPkt(SWITCH_BUFFER + RANDOM_BUFFER), *eventlist, queueLogger, memFromPkt(RANDOM_BUFFER));
    else if (qt==COMPOSITE)
	return new CompositeQueue(speedFromMbps(speed), queuesize, *eventlist, queueLogger);
    else if (qt==CTRL_PRIO)
	return new CtrlPrioQueue(speedFromMbps(speed), queuesize, *eventlist, queueLogger);
    else if (qt==ECN)
	return new ECNQueue(speedFromMbps(speed), memFromPkt(50*SWITCH_BUFFER), *eventlist, queueLogger, memFromPkt(10000));
    else if (qt==LOSSLESS)
	return new LosslessQueue(speedFromMbps(speed), memFromPkt(50), *eventlist, queueLogger, NULL);
    else if (qt==LOSSLESS_INPUT)
	return new LosslessOutputQueue(speedFromMbps(speed), memFromPkt(200), *eventlist, queueLogger);    
    else if (qt==LOSSLESS_INPUT_ECN)
	return new LosslessOutputQueue(speedFromMbps(speed), memFromPkt(10000), *eventlist, queueLogger,1,memFromPkt(16));
    assert(0);
}

void OverSubscribedFatTree::init_network(){
  QueueLoggerSampling* queueLogger = nullptr;

  // core to upper pod:
  for (int j=0;j<Nc;j++)
    for (int k=0;k<Nup;k++){
      queues_nc_nup[j][k] = NULL;
      pipes_nc_nup[j][k] = NULL;
      queues_nup_nc[k][j] = NULL;
      pipes_nup_nc[k][j] = NULL;
    }

  // upper pod to lower pod:
  for (int j=0;j<Nup;j++)
    for (int k=0;k<Nlp;k++){
      queues_nup_nlp[j][k] = NULL;
      pipes_nup_nlp[j][k] = NULL;
      queues_nlp_nup[k][j] = NULL;
      pipes_nlp_nup[k][j] = NULL;
    }
  
  // lower pod to host
  for (int j=0;j<Nlp;j++)
    for (int k=0;k<Nh;k++){
      queues_nlp_ns[j][k] = NULL;
      pipes_nlp_ns[j][k] = NULL;
      queues_ns_nlp[k][j] = NULL;
      pipes_ns_nlp[k][j] = NULL;
    }

  //create switches if we have lossless operation
  /*if (qt==LOSSLESS)
      for (int j=0;j<NK;j++){
	  switches_lp[j] = new Switch("Switch_LowerPod_"+ntoa(j));
	  switches_up[j] = new Switch("Switch_UpperPod_"+ntoa(j));
	  if (j<NC)
	      switches_c[j] = new Switch("Switch_Core_"+ntoa(j));
      }*/

  // links from lower layer pod switch to host
  for (int j = 0; j < Nlp; j++) {
    for (int l = 0; l < Nhpr; l++) {
  	  int k = j * Nhpr + l;
  	  // Downlink
  	  // queueLogger = nullptr; //new QueueLoggerSampling(timeFromMs(1000), *eventlist);
  	  //queueLogger = NULL;
  	  // logfile->addLogger(*queueLogger);
  	  
  	  queues_nlp_ns[j][k] = alloc_queue(queueLogger, _queuesize);
  	  queues_nlp_ns[j][k]->setName("LS" + ntoa(j) + "->DST" +ntoa(k));
  	  // logfile->writeName(*(queues_nlp_ns[j][k]));

  	  pipes_nlp_ns[j][k] = new Pipe(timeFromNs(RTT_rack), *eventlist);
  	  pipes_nlp_ns[j][k]->setName("Pipe-LS" + ntoa(j)  + "->DST" + ntoa(k));
  	  // logfile->writeName(*(pipes_nlp_ns[j][k]));

      pipes_nlp_ns[j][k]->set_pipe_downlink(); // modification - set this for the UtilMonitor

  	  // Uplink
  	  queueLogger = new QueueLoggerSampling(timeFromMs(1000), *eventlist);
  	  // logfile->addLogger(*queueLogger);
  	  queues_ns_nlp[k][j] = alloc_src_queue(queueLogger);
  	  queues_ns_nlp[k][j]->setName("SRC" + ntoa(k) + "->LS" +ntoa(j));
  	  // logfile->writeName(*(queues_ns_nlp[k][j]));

      if (qt==LOSSLESS){
        switches_lp[j]->addPort(queues_nlp_ns[j][k]);
        ((LosslessQueue*)queues_nlp_ns[j][k])->setRemoteEndpoint(queues_ns_nlp[k][j]);
      } else if (qt==LOSSLESS_INPUT || qt == LOSSLESS_INPUT_ECN){
        //no virtual queue needed at server
        new LosslessInputQueue(*eventlist,queues_ns_nlp[k][j]);
      }
  
  	  pipes_ns_nlp[k][j] = new Pipe(timeFromNs(RTT_rack), *eventlist);
  	  pipes_ns_nlp[k][j]->setName("Pipe-SRC" + ntoa(k) + "->LS" + ntoa(j));
  	  // logfile->writeName(*(pipes_ns_nlp[k][j]));
  
  	  if (ff){
  	    ff->add_queue(queues_nlp_ns[j][k]);
  	    ff->add_queue(queues_ns_nlp[k][j]);
  	  }
    }
  }

    /*    for (int i = 0;i<NSRV;i++){
      for (int j = 0;j<NK;j++){
	printf("%p/%p ",queues_ns_nlp[i][j], queues_nlp_ns[j][i]);
      }
      printf("\n");
      }*/
    
  //Lower layer in pod to upper layer in pod!
  for (int k = 0; k < Nup; k++) { // sweep upper pod switch id
    for (int l = 0; l < K/2; l++) { // sweep lower pod

      int pod = k / (K - Nhpr); // get the current pod
      int j = pod * K/2 + l;    // !!! there are K/2 ToRs per pod

      // Downlink
      // queueLogger = new QueueLoggerSampling(timeFromMs(1000), *eventlist);
      // logfile->addLogger(*queueLogger);
      queues_nup_nlp[k][j] = alloc_queue(queueLogger, SPEED, _queuesize);
      queues_nup_nlp[k][j]->setName("US" + ntoa(k) + "->LS" + ntoa(j));
      // logfile->writeName(*(queues_nup_nlp[k][j]));
      
      pipes_nup_nlp[k][j] = new Pipe(timeFromNs(RTT_net), *eventlist);
      pipes_nup_nlp[k][j]->setName("Pipe-US" + ntoa(k) + "->LS" + ntoa(j));
      // logfile->writeName(*(pipes_nup_nlp[k][j]));
      
      // Uplink
      // queueLogger = new QueueLoggerSampling(timeFromMs(1000), *eventlist);
      // logfile->addLogger(*queueLogger);
      queues_nlp_nup[j][k] = alloc_queue(queueLogger, SPEED, _queuesize);
      queues_nlp_nup[j][k]->setName("LS" + ntoa(j) + "->US" + ntoa(k));
      // logfile->writeName(*(queues_nlp_nup[j][k]));

      pipes_nlp_nup[j][k] = new Pipe(timeFromNs(RTT_net), *eventlist);
      pipes_nlp_nup[j][k]->setName("Pipe-LS" + ntoa(j) + "->US" + ntoa(k));
      // logfile->writeName(*(pipes_nlp_nup[j][k]));

      // if (qt==LOSSLESS){
  //     switches_lp[j]->addPort(queues_nlp_nup[j][k]);
  //     ((LosslessQueue*)queues_nlp_nup[j][k])->setRemoteEndpoint(queues_nup_nlp[k][j]);
  //     switches_up[k]->addPort(queues_nup_nlp[k][j]);
  //     ((LosslessQueue*)queues_nup_nlp[k][j])->setRemoteEndpoint(queues_nlp_nup[j][k]);
  // }else if (qt==LOSSLESS_INPUT || qt == LOSSLESS_INPUT_ECN){     
  //     new LosslessInputQueue(*eventlist, queues_nlp_nup[j][k]);
  //     new LosslessInputQueue(*eventlist, queues_nup_nlp[k][j]);
  // }
  
  // if (ff){
  //   ff->add_queue(queues_nlp_nup[j][k]);
  //   ff->add_queue(queues_nup_nlp[k][j]);
  //  }

    }
  }

    /*for (int i = 0;i<NK;i++){
      for (int j = 0;j<NK;j++){
	printf("%p/%p ",queues_nlp_nup[i][j], queues_nup_nlp[j][i]);
      }
      printf("\n");
      }*/
    
  // Upper layer in pod to core!
  for (int j = 0; j < Nup; j++) { // sweep upper pod switch ID

    int pod = j / (K - Nhpr); // get current pod
    int switch_pos = j % (K - Nhpr); // get switch position within pod

    for (int l = 0; l < K/2; l++) {

      int k = switch_pos * K/2 + l; // get the core switch ID

  	  // Downlink
    	// queueLogger = new QueueLoggerSampling(timeFromMs(1000), *eventlist);
    	// logfile->addLogger(*queueLogger);

    	queues_nup_nc[j][k] = alloc_queue(queueLogger, SPEED, _queuesize);
    	queues_nup_nc[j][k]->setName("US" + ntoa(j) + "->CS" + ntoa(k));
    	// logfile->writeName(*(queues_nup_nc[j][k]));
    	
    	pipes_nup_nc[j][k] = new Pipe(timeFromNs(RTT_net), *eventlist);
    	pipes_nup_nc[j][k]->setName("Pipe-US" + ntoa(j) + "->CS" + ntoa(k));
    	// logfile->writeName(*(pipes_nup_nc[j][k]));
	
    	// Uplink
    	// queueLogger = new QueueLoggerSampling(timeFromMs(1000), *eventlist);
    	// logfile->addLogger(*queueLogger);
    	
    	// if ((l+j*K/2)<failed_links){
    	//     queues_nc_nup[k][j] = alloc_queue(queueLogger,SPEED/10, _queuesize);
    	//   cout << "Adding link failure for j" << ntoa(j) << " l " << ntoa(l) << endl;
    	// }
     // 	else
      queues_nc_nup[k][j] = alloc_queue(queueLogger, SPEED, _queuesize);
    	queues_nc_nup[k][j]->setName("CS" + ntoa(k) + "->US" + ntoa(j));
      // logfile->writeName(*(queues_nc_nup[k][j]));
      
      pipes_nc_nup[k][j] = new Pipe(timeFromNs(RTT_net), *eventlist);
      pipes_nc_nup[k][j]->setName("Pipe-CS" + ntoa(k) + "->US" + ntoa(j));
      // logfile->writeName(*(pipes_nc_nup[k][j]));

	// if (qt==LOSSLESS){
	//     switches_up[j]->addPort(queues_nup_nc[j][k]);
	//     ((LosslessQueue*)queues_nup_nc[j][k])->setRemoteEndpoint(queues_nc_nup[k][j]);
	//     switches_c[k]->addPort(queues_nc_nup[k][j]);
	//     ((LosslessQueue*)queues_nc_nup[k][j])->setRemoteEndpoint(queues_nup_nc[j][k]);
	// }
	// else if (qt == LOSSLESS_INPUT || qt == LOSSLESS_INPUT_ECN){
	//     new LosslessInputQueue(*eventlist, queues_nup_nc[j][k]);
	//     new LosslessInputQueue(*eventlist, queues_nc_nup[k][j]);
	// }

  // if (ff){
  //   ff->add_queue(queues_nup_nc[j][k]);
  //   ff->add_queue(queues_nc_nup[k][j]);
  // }

    }
  }

    /*    for (int i = 0;i<NK;i++){
      for (int j = 0;j<NC;j++){
	printf("%p/%p ",queues_nup_nc[i][j], queues_nc_nup[j][i]);
      }
      printf("\n");
      }*/
    
    //init thresholds for lossless operation
  if (qt==LOSSLESS) {
    for (int j=0;j<Nlp;j++)
	    switches_lp[j]->configureLossless();
    for (int j=0;j<Nup;j++)
	    switches_up[j]->configureLossless();
    for (int j=0;j<Nc;j++)
        switches_c[j]->configureLossless();
  }
}

void check_non_null(Route* rt){
  int fail = 0;
  for (unsigned int i=1;i<rt->size()-1;i+=2)
    if (rt->at(i)==NULL){
      fail = 1;
      break;
    }
  
  if (fail){
    //    cout <<"Null queue in route"<<endl;
    for (unsigned int i=1;i<rt->size()-1;i+=2)
      printf("%p ",rt->at(i));

    cout<<endl;
    assert(0);
  }
}

vector<const Route*>* OverSubscribedFatTree::get_paths(int src, int dest){
  vector<const Route*>* paths = new vector<const Route*>();

  route_t *routeout, *routeback;
  QueueLoggerSimple *simplequeuelogger = new QueueLoggerSimple();
  //QueueLoggerSimple *simplequeuelogger = 0;
  // logfile->addLogger(*simplequeuelogger);
  //Queue* pqueue = new Queue(speedFromMbps((uint64_t)SPEED), memFromPkt(FEEDER_BUFFER), *eventlist, simplequeuelogger);
  //pqueue->setName("PQueue_" + ntoa(src) + "_" + ntoa(dest));
  // logfile->writeName(*pqueue);

  if (src / Nhpr == dest / Nhpr) { // within the same rack
  
    // cerr << "same rack!" << endl;
    // forward path
    routeout = new Route();
    //routeout->push_back(pqueue);
    routeout->push_back(queues_ns_nlp[src][src / Nhpr]);
    routeout->push_back(pipes_ns_nlp[src][src / Nhpr]);

    // if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
    //   routeout->push_back(queues_ns_nlp[src][HOST_POD_SWITCH(src)]->getRemoteEndpoint());

    routeout->push_back(queues_nlp_ns[dest / Nhpr][dest]);
    routeout->push_back(pipes_nlp_ns[dest / Nhpr][dest]);

    // reverse path for RTS packets
    routeback = new Route();
    routeback->push_back(queues_ns_nlp[dest][dest / Nhpr]);
    routeback->push_back(pipes_ns_nlp[dest][dest / Nhpr]);

    // if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
    //   routeback->push_back(queues_ns_nlp[dest][HOST_POD_SWITCH(dest)]->getRemoteEndpoint());

    routeback->push_back(queues_nlp_ns[src / Nhpr][src]);
    routeback->push_back(pipes_nlp_ns[src / Nhpr][src]);

    routeout->set_reverse(routeback);
    routeback->set_reverse(routeout);

    //print_route(*routeout);
    paths->push_back(routeout);

    check_non_null(routeout);
    return paths;
  }
  else if (src / (Nhpr * K/2) == dest / (Nhpr * K/2)) { // within the same pod
    //don't go up the hierarchy, stay in the pod only.
    // cerr << "same pod! " << endl;

    int pod = src / (Nhpr * K/2); // get the pod

    //there are (K - Nhpr) paths between the source and the destination
    for (int u = 0; u < (K - Nhpr); u++){
      // upper is nup
    // cerr << u << endl;

      int upper = pod * (K - Nhpr) + u; // get the upper switch ID
      
      routeout = new Route();
      //routeout->push_back(pqueue);
      
      routeout->push_back(queues_ns_nlp[src][src / Nhpr]);
      routeout->push_back(pipes_ns_nlp[src][src / Nhpr]);

      // if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      //   routeout->push_back(queues_ns_nlp[src][HOST_POD_SWITCH(src)]->getRemoteEndpoint());

      routeout->push_back(queues_nlp_nup[src / Nhpr][upper]);
      routeout->push_back(pipes_nlp_nup[src / Nhpr][upper]);

      // if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      //   routeout->push_back(queues_nlp_nup[HOST_POD_SWITCH(src)][upper]->getRemoteEndpoint());

      routeout->push_back(queues_nup_nlp[upper][dest / Nhpr]);
      routeout->push_back(pipes_nup_nlp[upper][dest / Nhpr]);

      // if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      //   routeout->push_back(queues_nup_nlp[upper][HOST_POD_SWITCH(dest)]->getRemoteEndpoint());

      routeout->push_back(queues_nlp_ns[dest / Nhpr][dest]);
      routeout->push_back(pipes_nlp_ns[dest / Nhpr][dest]);

      // reverse path for RTS packets
      routeback = new Route();
      
      routeback->push_back(queues_ns_nlp[dest][dest / Nhpr]);
      routeback->push_back(pipes_ns_nlp[dest][dest / Nhpr]);

      // if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      //   routeback->push_back(queues_ns_nlp[dest][HOST_POD_SWITCH(dest)]->getRemoteEndpoint());

      routeback->push_back(queues_nlp_nup[dest / Nhpr][upper]);
      routeback->push_back(pipes_nlp_nup[dest / Nhpr][upper]);

      // if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      //   routeback->push_back(queues_nlp_nup[HOST_POD_SWITCH(dest)][upper]->getRemoteEndpoint());

      routeback->push_back(queues_nup_nlp[upper][src / Nhpr]);
      routeback->push_back(pipes_nup_nlp[upper][src / Nhpr]);

      // if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      //   routeback->push_back(queues_nup_nlp[upper][HOST_POD_SWITCH(src)]->getRemoteEndpoint());
      
      routeback->push_back(queues_nlp_ns[src / Nhpr][src]);
      routeback->push_back(pipes_nlp_ns[src / Nhpr][src]);

      routeout->set_reverse(routeback);
      routeback->set_reverse(routeout);
      
      //print_route(*routeout);
      paths->push_back(routeout);
      check_non_null(routeout);
    }
    return paths;
  }
  else { // not in the same pod

      // cerr << "diff pod! " << endl;
    int pod = src / (Nhpr * K/2); // get the pod

    for (int u = 0; u < (K - Nhpr); u++) {

      int upper = pod * (K - Nhpr) + u; // get the upper switch ID

      for (int c = 0; c < (K/2); c++){

        int core = u * (K/2) + c; // get the core switch ID
        
        // upper is nup
	
      	routeout = new Route();
      	//routeout->push_back(pqueue);
      	
      	routeout->push_back(queues_ns_nlp[src][src / Nhpr]);
      	routeout->push_back(pipes_ns_nlp[src][src / Nhpr]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
       //    routeout->push_back(queues_ns_nlp[src][HOST_POD_SWITCH(src)]->getRemoteEndpoint());
      	
      	routeout->push_back(queues_nlp_nup[src / Nhpr][upper]);
      	routeout->push_back(pipes_nlp_nup[src / Nhpr][upper]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeout->push_back(queues_nlp_nup[HOST_POD_SWITCH(src)][upper]->getRemoteEndpoint());
      	
      	routeout->push_back(queues_nup_nc[upper][core]);
      	routeout->push_back(pipes_nup_nc[upper][core]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeout->push_back(queues_nup_nc[upper][core]->getRemoteEndpoint());
      	
      	//now take the only link down to the destination server!
      	
        int pod2 = dest / (Nhpr * K/2); // get the pod
      	int upper2 = pod2 * (K - Nhpr) + u;
      	//printf("K %d HOST_POD(%d) %d core %d upper2 %d\n",K,dest,HOST_POD(dest),core, upper2);
      	
      	routeout->push_back(queues_nc_nup[core][upper2]);
      	routeout->push_back(pipes_nc_nup[core][upper2]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeout->push_back(queues_nc_nup[core][upper2]->getRemoteEndpoint());	

      	routeout->push_back(queues_nup_nlp[upper2][dest / Nhpr]);
      	routeout->push_back(pipes_nup_nlp[upper2][dest / Nhpr]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeout->push_back(queues_nup_nlp[upper2][HOST_POD_SWITCH(dest)]->getRemoteEndpoint());
      	
      	routeout->push_back(queues_nlp_ns[dest / Nhpr][dest]);
      	routeout->push_back(pipes_nlp_ns[dest / Nhpr][dest]);

      	// reverse path for RTS packets
      	routeback = new Route();
      	
      	routeback->push_back(queues_ns_nlp[dest][dest / Nhpr]);
      	routeback->push_back(pipes_ns_nlp[dest][dest / Nhpr]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeback->push_back(queues_ns_nlp[dest][HOST_POD_SWITCH(dest)]->getRemoteEndpoint());
      	
      	routeback->push_back(queues_nlp_nup[dest / Nhpr][upper2]);
      	routeback->push_back(pipes_nlp_nup[dest / Nhpr][upper2]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeback->push_back(queues_nlp_nup[HOST_POD_SWITCH(dest)][upper2]->getRemoteEndpoint());
      	
      	routeback->push_back(queues_nup_nc[upper2][core]);
      	routeback->push_back(pipes_nup_nc[upper2][core]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeback->push_back(queues_nup_nc[upper2][core]->getRemoteEndpoint());
      	
      	//now take the only link back down to the src server!
      	
      	routeback->push_back(queues_nc_nup[core][upper]);
      	routeback->push_back(pipes_nc_nup[core][upper]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeback->push_back(queues_nc_nup[core][upper]->getRemoteEndpoint());
      	
      	routeback->push_back(queues_nup_nlp[upper][src / Nhpr]);
      	routeback->push_back(pipes_nup_nlp[upper][src / Nhpr]);

      	// if (qt==LOSSLESS_INPUT || qt==LOSSLESS_INPUT_ECN)
      	//     routeback->push_back(queues_nup_nlp[upper][HOST_POD_SWITCH(src)]->getRemoteEndpoint());
      	
      	routeback->push_back(queues_nlp_ns[src / Nhpr][src]);
      	routeback->push_back(pipes_nlp_ns[src / Nhpr][src]);


      	routeout->set_reverse(routeback);
      	routeback->set_reverse(routeout);
      	
      	//print_route(*routeout);
      	paths->push_back(routeout);
      	check_non_null(routeout);
      }
    }
    return paths;
  }
}

void OverSubscribedFatTree::count_queue(Queue* queue){
  if (_link_usage.find(queue)==_link_usage.end()){
    _link_usage[queue] = 0;
  }

  _link_usage[queue] = _link_usage[queue] + 1;
}

int OverSubscribedFatTree::find_lp_switch(Queue* queue){
  //first check ns_nlp
  for (int i=0;i<Nh;i++)
    for (int j = 0;j<Nlp;j++)
      if (queues_ns_nlp[i][j]==queue)
	return j;

  //only count nup to nlp
  count_queue(queue);

  for (int i=0;i<Nup;i++)
    for (int j = 0;j<Nlp;j++)
      if (queues_nup_nlp[i][j]==queue)
	return j;

  return -1;
}

int OverSubscribedFatTree::find_up_switch(Queue* queue){
  count_queue(queue);
  //first check nc_nup
  for (int i=0;i<Nc;i++)
    for (int j = 0;j<Nup;j++)
      if (queues_nc_nup[i][j]==queue)
	return j;

  //check nlp_nup
  for (int i=0;i<Nlp;i++)
    for (int j = 0;j<Nup;j++)
      if (queues_nlp_nup[i][j]==queue)
	return j;

  return -1;
}

int OverSubscribedFatTree::find_core_switch(Queue* queue){
  count_queue(queue);
  //first check nup_nc
  for (int i=0;i<Nup;i++)
    for (int j = 0;j<Nc;j++)
      if (queues_nup_nc[i][j]==queue)
	return j;

  return -1;
}

int OverSubscribedFatTree::find_destination(Queue* queue){
  //first check nlp_ns
  for (int i=0;i<Nlp;i++)
    for (int j = 0;j<Nh;j++)
      if (queues_nlp_ns[i][j]==queue)
	return j;

  return -1;
}

void OverSubscribedFatTree::print_path(std::ofstream &paths,int src,const Route* route){
  paths << "SRC_" << src << " ";
  
  if (route->size()/2==2){
    paths << "LS_" << find_lp_switch((Queue*)route->at(1)) << " ";
    paths << "DST_" << find_destination((Queue*)route->at(3)) << " ";
  } else if (route->size()/2==4){
    paths << "LS_" << find_lp_switch((Queue*)route->at(1)) << " ";
    paths << "US_" << find_up_switch((Queue*)route->at(3)) << " ";
    paths << "LS_" << find_lp_switch((Queue*)route->at(5)) << " ";
    paths << "DST_" << find_destination((Queue*)route->at(7)) << " ";
  } else if (route->size()/2==6){
    paths << "LS_" << find_lp_switch((Queue*)route->at(1)) << " ";
    paths << "US_" << find_up_switch((Queue*)route->at(3)) << " ";
    paths << "CS_" << find_core_switch((Queue*)route->at(5)) << " ";
    paths << "US_" << find_up_switch((Queue*)route->at(7)) << " ";
    paths << "LS_" << find_lp_switch((Queue*)route->at(9)) << " ";
    paths << "DST_" << find_destination((Queue*)route->at(11)) << " ";
  } else {
    paths << "Wrong hop count " << ntoa(route->size()/2);
  }
  
  paths << endl;
}





//////////////////////////////////////////////
//      Aggregate utilization monitor       //
//////////////////////////////////////////////


// UtilMonitor::UtilMonitor(OverSubscribedFatTree* top, EventList &eventlist)
//   : EventSource(eventlist,"utilmonitor"), _top(top)
// {
//     _H = _top->no_of_nodes(); // number of hosts
//     _N = _top->K * _top->K / 2; // racks
//     _hpr = _top->Nhpr; // hosts per rack
//     uint64_t rate = speedFromMbps((uint64_t)SPEED) / 8; // bytes / second
//     rate = rate * _H;
//     //rate = rate / 1500; // total packets per second

//     _max_agg_Bps = rate;

//     // debug:
//     //cout << "max packets per second = " << rate << endl;

// }

// void UtilMonitor::start(simtime_picosec period) {
//     _period = period;
//     _max_B_in_period = _max_agg_Bps * timeAsSec(_period);

//     // debug:
//     //cout << "_max_pkts_in_period = " << _max_pkts_in_period << endl;

//     eventlist().sourceIsPending(*this, _period);
// }

// void UtilMonitor::doNextEvent() {
//     printAggUtil();
// }

// void UtilMonitor::printAggUtil() {

//     uint64_t B_sum = 0;

//     int host = 0;
//     for (int tor = 0; tor < _N; tor++) {
//         for (int downlink = 0; downlink < _hpr; downlink++) {
//             Pipe* pipe = _top->get_downlink(tor, host);
//             B_sum = B_sum + pipe->reportBytes();
//             host++;
//         }
//     }

//     // debug:
//     //cout << "Bsum = " << B_sum << endl;
//     //cout << "_max_B_in_period = " << _max_B_in_period << endl;

//     double util = (double)B_sum / (double)_max_B_in_period;

//     fct_util_out << "Util " << fixed << util << " " << timeAsMs(eventlist().now()) << endl;

//     //if (eventlist().now() + _period < eventlist().getEndtime())
//     eventlist().sourceIsPendingRel(*this, _period);

// }
