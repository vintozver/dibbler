/*                                                                           
 * Dibbler - a portable DHCPv6                                               
 *                                                                           
 * authors: Tomasz Mrugalski <thomson@klub.com.pl>                           
 *          Marek Senderski <msend@o2.pl>                                    
 * changes: Michal Kowalczuk <michal@kowalczuk.eu>
 *                                                                           
 * released under GNU GPL v2 only licence                                
 *                                                                           
 * $Id: SrvMsgLeaseQueryReply.cpp,v 1.7 2008-08-29 00:07:35 thomson Exp $
 */

#include "SrvMsgLeaseQueryReply.h"
#include "Logger.h"
#include "SrvOptLQ.h"
#include "SrvOptStatusCode.h" 
#include "SrvOptIAAddress.h"
#include "SrvOptServerIdentifier.h"
#include "SrvOptIAPrefix.h"
#include "SrvOptClientIdentifier.h"
#include "AddrClient.h"
#include "SrvCfgMgr.h"
#include "OptStringLst.h"
#include "SrvGeolocMgr.h"


TSrvMsgLeaseQueryReply::TSrvMsgLeaseQueryReply(SPtr<TSrvMsgLeaseQuery> query)
    :TSrvMsg(query->getIface(), query->getAddr(), LEASEQUERY_REPLY_MSG,
	     query->getTransID())
{
    if (!answer(query)) {
        Log(Error) << "LQ: LQ-QUERY response generation failed." << LogEnd;
        IsDone = true;
    } else {
        Log(Debug) << "LQ: LQ-QUERY response generation successful." << LogEnd;
        IsDone = false;
    }
}


/** 
 * 
 * 
 * @param queryMsg 
 * 
 * @return true - answer should be sent
 */
bool TSrvMsgLeaseQueryReply::answer(SPtr<TSrvMsgLeaseQuery> queryMsg) {

    int count = 0;
    SPtr<TOpt> opt;
    bool ok = true;
    bool withGeoloc = false;

    Log(Info) << "LQ: Generating new LEASEQUERY_RESP message." << LogEnd;
    
    queryMsg->firstOption();
    while ( opt = queryMsg->getOption()) {
	switch (opt->getOptType()) {
	case OPTION_LQ_QUERY:
	{
	    count++;
	    SPtr<TSrvOptLQ> q = (Ptr*) opt;
	    switch (q->getQueryType()) {
	    case QUERY_BY_ADDRESS:
		ok = queryByAddress(q, queryMsg, withGeoloc);
		break;
            case QUERY_BY_ADDRESS_WITH_GEOLOC:
                withGeoloc = true;
		ok = queryByAddress(q, queryMsg, withGeoloc);
		break;
	    case QUERY_BY_CLIENTID:
		ok = queryByClientID(q, queryMsg, withGeoloc);
		break;
            case QUERY_BY_CLIENTID_WITH_GEOLOC:
                withGeoloc = true;
                ok = queryByClientID(q, queryMsg, withGeoloc);
                break;
	    default:
		Options.push_back( new TSrvOptStatusCode(STATUSCODE_UNKNOWNQUERYTYPE, "Invalid Query type.", this) );
		Log(Warning) << "LQ: Invalid query type (" << q->getQueryType() << " received." << LogEnd;
		return true;
	    }
	    if (!ok) {
		Log(Warning) << "LQ: Malformed query detected." << LogEnd;
		return false;
	    }
	    break;
	}
	case OPTION_CLIENTID:
	    // copy the client-id option
	    Options.push_back(opt);
	    break;
	}

    }
    if (!count) {
	Options.push_back(new TSrvOptStatusCode(STATUSCODE_MALFORMEDQUERY, "Required LQ_QUERY option missing.", this));
	return true;
    }

    // append SERVERID
    SPtr<TSrvOptServerIdentifier> ptrSrvID;
    ptrSrvID = new TSrvOptServerIdentifier(SrvCfgMgr().getDUID(), this);
    Options.push_back((Ptr*)ptrSrvID);
   
    // allocate buffer
    pkt = new char[getSize()];
    this->send();

    return true;
}

bool TSrvMsgLeaseQueryReply::queryByAddress(SPtr<TSrvOptLQ> q, SPtr<TSrvMsgLeaseQuery> queryMsg, bool withGeoloc) {
    SPtr<TOpt> opt;
    q->firstOption();
    SPtr<TSrvOptIAAddress> addr = 0;
    SPtr<TIPv6Addr> link = q->getLinkAddr();
    
    while ( opt = q->getOption() ) {
	if (opt->getOptType() == OPTION_IAADDR)
	    addr = (Ptr*) opt;
    }
    if (!addr) {
	Options.push_back(new TSrvOptStatusCode(STATUSCODE_MALFORMEDQUERY, "Required IAADDR suboption missing.", this));
	return true;
    }

    // search for client
    SPtr<TAddrClient> cli = SrvAddrMgr().getClient( addr->getAddr() );
    
    if (!cli) {
	Log(Warning) << "LQ: Assignement for client addr=" << addr->getAddr()->getPlain() << " not found." << LogEnd;
	Options.push_back( new TSrvOptStatusCode(STATUSCODE_NOTCONFIGURED, "No binding for this address found.", this) );
	return true;
    }
    
    appendClientData(cli);
    // if geolocation information needed, append it to message
    if(withGeoloc) {
        appendGeolocInfo(cli);
    }
    return true;
}

bool TSrvMsgLeaseQueryReply::queryByClientID(SPtr<TSrvOptLQ> q, SPtr<TSrvMsgLeaseQuery> queryMsg, bool withGeoloc) {
    SPtr<TOpt> opt;
    SPtr<TSrvOptClientIdentifier> duidOpt = 0;
    SPtr<TDUID> duid = 0;
    SPtr<TIPv6Addr> link = q->getLinkAddr();
    
    q->firstOption();
    while ( opt = q->getOption() ) {
	if (opt->getOptType() == OPTION_CLIENTID) {
	    duidOpt = (Ptr*) opt;
	    duid = duidOpt->getDUID();
	}
    }
    if (!duid) {
	Options.push_back( new TSrvOptStatusCode(STATUSCODE_UNSPECFAIL, "You didn't send your ClientID.", this) );
	return true;
    }

    // search for client
    SPtr<TAddrClient> cli = SrvAddrMgr().getClient( duid );
    
    if (!cli) {
	Log(Warning) << "LQ: Assignement for client duid=" << duid->getPlain() << " not found." << LogEnd;
	Options.push_back( new TSrvOptStatusCode(STATUSCODE_NOTCONFIGURED, "No binding for this DUID found.", this) );
	return true;
    }
    
    appendClientData(cli);
    // if geolocation information needed, append it to message
    if(withGeoloc) {
        appendGeolocInfo(cli);
    }
    return true;
}

/*
 * Appending geolocation information to LeaseQuery Reply message.
 * Appending is based on client's DUID (no matter if LeaseQuery request was by
 * address or by DUID).
 * 
 * Since there was a problem with reading coordinates (on requestor side) with dots,
 * dots are replaced by commas.
 */
void TSrvMsgLeaseQueryReply::appendGeolocInfo(SPtr<TAddrClient> cli) {
    
    List(string) coordinates = SrvGeolocMgr().getGeolocInfo(cli->getDUID());
    
    if(coordinates.count()) {
        Log(Debug) << "LQ: Adding geolocation information." << LogEnd;
        
        SPtr<string> s;
        List(string) coordinatesEnhanced;
        
        coordinates.first();
        while (s = coordinates.get()) {
                const char * c = s->c_str();
                string newCoordinate = c;
                newCoordinate.replace(newCoordinate.find("."), 1, ",");
                s = new string(newCoordinate);
                coordinatesEnhanced.append(s);
        }
        SPtr<TOpt> ptr = new TOptStringLst(OPTION_GEOLOC, coordinatesEnhanced, this);
        Options.push_back(ptr);
    } else {
        Log(Warning) << "LQ: Geolocation information for duid=";
        Log(Cont) << cli->getDUID()->getPlain() << " not found." << LogEnd;
	Options.push_back(new TSrvOptStatusCode(STATUSCODE_NOTCONFIGURED, "No geolcation information for this DUID found.", this));
    }
}

void TSrvMsgLeaseQueryReply::appendClientData(SPtr<TAddrClient> cli) {

    Log(Debug) << "LQ: Appending data for client " << cli->getDUID()->getPlain() << LogEnd;

    SPtr<TSrvOptLQClientData> cliData = new TSrvOptLQClientData(this);
    
    SPtr<TAddrIA> ia;
    SPtr<TAddrAddr> addr;
    SPtr<TAddrPrefix> prefix;

    unsigned long nowTs = now();
    unsigned long cliTs = cli->getLastTimestamp();
    unsigned long diff = nowTs - cliTs;

    Log(Debug) << "LQ: modifying the lifetimes (client last seen " << diff << "secs ago)." << LogEnd;

    // add all assigned addresses
    cli->firstIA();
    while ( ia = cli->getIA() ) {
	ia->firstAddr();
	while ( addr=ia->getAddr() ) {
	    unsigned long a = addr->getPref() - diff;
	    unsigned long b = addr->getValid() - diff;
	    cliData->addOption( new TSrvOptIAAddress(addr->get(), a, b, this) );
	}
    }

    // add all assigned prefixes
    cli->firstPD();
    while ( ia = cli->getPD() ) {
	ia->firstPrefix();
	while (prefix = ia->getPrefix()) {
	    cliData->addOption( new TSrvOptIAPrefix( prefix->getPrefix(), prefix->getLength(), prefix->getPref(),
						     prefix->getValid(), this));
	}
    }

    cliData->addOption(new TSrvOptClientIdentifier(cli->getDUID(), this));

    // TODO: add all temporary addresses

    // add CLT_TIME
    Log(Debug) << "LQ: Adding CLT_TIME option: " << diff << " second(s)." << LogEnd;

    cliData->addOption( new TSrvOptLQClientTime(diff, this));

    Options.push_back((Ptr*)cliData);
}

bool TSrvMsgLeaseQueryReply::check() {
    // this should never happen
    return true;
}

TSrvMsgLeaseQueryReply::~TSrvMsgLeaseQueryReply() {
}

unsigned long TSrvMsgLeaseQueryReply::getTimeout() {
    return 0;
}
void TSrvMsgLeaseQueryReply::doDuties() {
    IsDone = true;
}

string TSrvMsgLeaseQueryReply::getName() {
    return "LEASE-QUERY-REPLY";
}
