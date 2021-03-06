/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <iostream>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/foreach.hpp>
#include <pugixml/pugixml.hpp>

#include <db/db.h>
#include <base/logging.h>

#include <cmn/agent_cmn.h>
#include <cmn/agent_factory.h>
#include <cmn/agent_stats.h>
#include <cmn/agent_param.h>

#include <cfg/cfg_init.h>
#include <cfg/discovery_agent.h>

#include <oper/operdb_init.h>
#include <controller/controller_init.h>
#include <pkt/pkt_init.h>
#include <services/services_init.h>
#include <ksync/ksync_init.h>
#include <uve/agent_uve.h>
#include <kstate/kstate.h>
#include <pkt/proto_handler.h>
#include <diag/diag.h>
#include <vgw/cfg_vgw.h>
#include <vgw/vgw.h>

#include "contrail_agent_init.h"

/****************************************************************************
 * Initialization routines
****************************************************************************/
// Initialization for VMWare specific interfaces
void ContrailAgentInit::InitVmwareInterface() {
    if (!params_->isVmwareMode())
        return;

    PhysicalInterface::Create(agent_->GetInterfaceTable(),
                              params_->vmware_physical_port(),
                              agent_->GetDefaultVrf());
}

void ContrailAgentInit::InitLogging() {
    Sandesh::SetLoggingParams(params_->log_local(),
                              params_->log_category(),
                              params_->log_level());
}

// Connect to collector specified in config, if discovery server is not set
void ContrailAgentInit::InitCollector() {
    agent_->InitCollector();
}

// Create the basic modules for agent operation.
// Optional modules or modules that have different implementation are created
// by init module
void ContrailAgentInit::CreateModules() {
    agent_->set_cfg(new AgentConfig(agent_));
    agent_->set_stats(new AgentStats(agent_));
    agent_->set_oper_db(new OperDB(agent_));
    agent_->set_uve(AgentObjectFactory::Create<AgentUve>(
                    agent_, AgentUve::kBandwidthInterval));
    agent_->set_ksync(AgentObjectFactory::Create<KSync>(agent_));
    agent_->set_pkt(new PktModule(agent_));

    agent_->set_services(new ServicesModule(agent_,
                                            params_->metadata_shared_secret()));
    agent_->set_vgw(new VirtualGateway(agent_));

    agent_->set_controller(new VNController(agent_));
}

void ContrailAgentInit::CreateDBTables() {
    agent_->CreateDBTables();
}

void ContrailAgentInit::CreateDBClients() {
    agent_->CreateDBClients();
    agent_->uve()->RegisterDBClients();
    agent_->ksync()->RegisterDBClients(agent_->GetDB());
    agent_->vgw()->RegisterDBClients();
}

void ContrailAgentInit::InitPeers() {
    agent_->InitPeers();
}

void ContrailAgentInit::InitModules() {
    agent_->InitModules();
    agent_->ksync()->Init(true);
    agent_->pkt()->Init(true);
    agent_->services()->Init(true);
    agent_->uve()->Init();
}

void ContrailAgentInit::CreateVrf() {
    // Create the default VRF
    VrfTable *vrf_table = agent_->GetVrfTable();

    if (agent_->isXenMode()) {
        vrf_table->CreateStaticVrf(agent_->GetLinkLocalVrfName());
    }
    vrf_table->CreateStaticVrf(agent_->GetDefaultVrf());

    VrfEntry *vrf = vrf_table->FindVrfFromName(agent_->GetDefaultVrf());
    assert(vrf);

    // Default VRF created; create nexthops
    agent_->SetDefaultInet4UnicastRouteTable(vrf->GetInet4UnicastRouteTable());
    agent_->SetDefaultInet4MulticastRouteTable
        (vrf->GetInet4MulticastRouteTable());
    agent_->SetDefaultLayer2RouteTable(vrf->GetLayer2RouteTable());

    // Create VRF for VGw
    agent_->vgw()->CreateVrf();
}

void ContrailAgentInit::CreateNextHops() {
    DiscardNH::Create();
    ResolveNH::Create();

    DiscardNHKey key;
    NextHop *nh = static_cast<NextHop *>
                (agent_->nexthop_table()->FindActiveEntry(&key));
    agent_->nexthop_table()->set_discard_nh(nh);
}

void ContrailAgentInit::CreateInterfaces() {
    InterfaceTable *table = agent_->GetInterfaceTable();

    InetInterface::Create(table, params_->vhost_name(), InetInterface::VHOST,
                          agent_->GetDefaultVrf(), params_->vhost_addr(),
                          params_->vhost_plen(), params_->vhost_gw(),
                          agent_->GetDefaultVrf());
    PhysicalInterface::Create(table, params_->eth_port(),
                              agent_->GetDefaultVrf());
    agent_->InitXenLinkLocalIntf();
    InitVmwareInterface();

    // Set VHOST interface
    InetInterfaceKey key(agent_->vhost_interface_name());
    agent_->set_vhost_interface
        (static_cast<Interface *>(table->FindActiveEntry(&key)));
    assert(agent_->vhost_interface());

    // Validate physical interface
    PhysicalInterfaceKey physical_key(agent_->GetIpFabricItfName());
    assert(table->FindActiveEntry(&physical_key));

    agent_->SetRouterId(params_->vhost_addr());
    agent_->SetPrefixLen(params_->vhost_plen());
    agent_->SetGatewayId(params_->vhost_gw());
    agent_->pkt()->CreateInterfaces();
    agent_->vgw()->CreateInterfaces();
}

void ContrailAgentInit::InitDiscovery() {
    agent_->cfg()->InitDiscovery();
}

void ContrailAgentInit::InitDone() {
    //Open up mirror socket
    agent_->GetMirrorTable()->MirrorSockInit();

    agent_->services()->ConfigInit();
    // Diag module needs PktModule
    agent_->set_diag_table(new DiagTable(agent_));
    //Update mac address of vhost interface with
    //that of ethernet interface
    agent_->ksync()->UpdateVhostMac();
    agent_->ksync()->VnswInterfaceListenerInit();

    if (agent_->GetRouterIdConfigured()) {
        RouterIdDepInit(agent_);
    } else {
        LOG(DEBUG, 
            "Router ID Dependent modules (Nova & BGP) not initialized");
    }

    agent_->cfg()->InitDone();
}

// Start init sequence
bool ContrailAgentInit::Run() {
    InitLogging();
    InitCollector();
    InitPeers();
    CreateModules();
    CreateDBTables();
    CreateDBClients();
    InitModules();
    CreateVrf();
    CreateNextHops();
    InitDiscovery();
    CreateInterfaces();
    InitDone();

    agent_->set_init_done(true);
    return true;
}

void ContrailAgentInit::Init(AgentParam *param, Agent *agent,
                     const boost::program_options::variables_map &var_map) {
    params_ = param;
    agent_ = agent;
}

// Trigger inititlization in context of DBTable
void ContrailAgentInit::Start() {
    if (params_->log_file() == "") {
        LoggingInit();
    } else {
        LoggingInit(params_->log_file());
    }

    params_->LogConfig();
    params_->Validate();

    int task_id = TaskScheduler::GetInstance()->GetTaskId("db::DBTable");
    trigger_.reset(new TaskTrigger(boost::bind(&ContrailAgentInit::Run, this), 
                                   task_id, 0));
    trigger_->Set();
    return;
}
