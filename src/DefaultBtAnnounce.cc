/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "DefaultBtAnnounce.h"
#include "LogFactory.h"
#include "Logger.h"
#include "DelegatingPeerListProcessor.h"
#include "Util.h"
#include "prefs.h"
#include "DlAbortEx.h"
#include "message.h"
#include "SimpleRandomizer.h"
#include "BtContext.h"
#include "PieceStorage.h"
#include "BtRuntime.h"
#include "PeerStorage.h"
#include "Peer.h"
#include "Option.h"
#include "StringFormat.h"
#include "A2STR.h"
#include "Request.h"
#include "bencode.h"

namespace aria2 {

DefaultBtAnnounce::DefaultBtAnnounce(const BtContextHandle& btContext,
				     const Option* option):
  btContext(btContext),
  trackers(0),
  interval(DEFAULT_ANNOUNCE_INTERVAL),
  minInterval(DEFAULT_ANNOUNCE_INTERVAL),
  complete(0),
  incomplete(0),
  announceList(btContext->getAnnounceTiers()),
  option(option),
  logger(LogFactory::getInstance()),
  _randomizer(SimpleRandomizer::getInstance())
{
  prevAnnounceTime.setTimeInSec(0);
  generateKey();
}

DefaultBtAnnounce::~DefaultBtAnnounce() {
}

void DefaultBtAnnounce::generateKey()
{
  key = Util::randomAlpha(8, _randomizer);
}

bool DefaultBtAnnounce::isDefaultAnnounceReady() {
  return (trackers == 0 && prevAnnounceTime.elapsed(minInterval) &&
	  !announceList.allTiersFailed());
}

bool DefaultBtAnnounce::isStoppedAnnounceReady() {
  return (trackers == 0 &&
	  btRuntime->isHalt() &&
	  announceList.countStoppedAllowedTier());
}

bool DefaultBtAnnounce::isCompletedAnnounceReady() {
  return (trackers == 0 &&
	  pieceStorage->allDownloadFinished() &&
	  announceList.countCompletedAllowedTier());
}

bool DefaultBtAnnounce::isAnnounceReady() {
  return
    isStoppedAnnounceReady() ||
    isCompletedAnnounceReady() ||
    isDefaultAnnounceReady();
}

static bool uriHasQuery(const std::string& uri)
{
  Request req;
  req.setUrl(uri);
  return !req.getQuery().empty();
}

std::string DefaultBtAnnounce::getAnnounceUrl() {
  if(isStoppedAnnounceReady()) {
    if(!announceList.currentTierAcceptsStoppedEvent()) {
      announceList.moveToStoppedAllowedTier();
    }
    announceList.setEvent(AnnounceTier::STOPPED);
  } else if(isCompletedAnnounceReady()) {
    if(!announceList.currentTierAcceptsCompletedEvent()) {
      announceList.moveToCompletedAllowedTier();
    }
    announceList.setEvent(AnnounceTier::COMPLETED);
  } else if(isDefaultAnnounceReady()) {
    // If download completed before "started" event is sent to a tracker,
    // we change the event to something else to prevent us from
    // sending "completed" event.
    if(pieceStorage->allDownloadFinished() &&
       announceList.getEvent() == AnnounceTier::STARTED) {
      announceList.setEvent(AnnounceTier::STARTED_AFTER_COMPLETION);
    }
  } else {
    return A2STR::NIL;
  }
  unsigned int numWant = 50;
  if(!btRuntime->lessThanMinPeers() || btRuntime->isHalt()) {
    numWant = 0;
  }
  TransferStat stat = peerStorage->calculateStat();
  uint64_t left =
    pieceStorage->getTotalLength()-pieceStorage->getCompletedLength();
  std::string url = announceList.getAnnounce();
  url += uriHasQuery(url) ? "&" : "?";
  url += 
    "info_hash="+Util::torrentUrlencode(btContext->getInfoHash(),
					btContext->getInfoHashLength())+
    "&peer_id="+Util::torrentUrlencode(btContext->getPeerId(), 20)+
    "&uploaded="+Util::uitos(stat.getSessionUploadLength())+
    "&downloaded="+Util::uitos(stat.getSessionDownloadLength())+
    "&left="+Util::uitos(left)+
    "&compact=1"+
    "&key="+key+
    "&numwant="+Util::uitos(numWant)+
    "&no_peer_id=1";
  if(btRuntime->getListenPort() > 0) {
    url += "&port="+Util::uitos(btRuntime->getListenPort());
  }
  std::string event = announceList.getEventString();
  if(!event.empty()) {
    url += "&event="+event;
  }
  if(!trackerId.empty()) {
    url += "&trackerid="+Util::torrentUrlencode(trackerId);
  }
  if(option->getAsBool(PREF_BT_REQUIRE_CRYPTO)) {
    url += "&requirecrypto=1";
  } else {
    url += "&supportcrypto=1";
  }
  return url;
}

void DefaultBtAnnounce::announceStart() {
  trackers++;
}

void DefaultBtAnnounce::announceSuccess() {
  trackers = 0;
  announceList.announceSuccess();
}

void DefaultBtAnnounce::announceFailure() {
  trackers = 0;
  announceList.announceFailure();
}

bool DefaultBtAnnounce::isAllAnnounceFailed() {
  return announceList.allTiersFailed();
}

void DefaultBtAnnounce::resetAnnounce() {
  prevAnnounceTime.reset();
  announceList.resetTier();
}

void
DefaultBtAnnounce::processAnnounceResponse(const unsigned char* trackerResponse,
					   size_t trackerResponseLength)
{
  logger->debug("Now processing tracker response.");
  const bencode::BDE dict =
    bencode::decode(trackerResponse, trackerResponseLength);
  if(!dict.isDict()) {
    throw DlAbortEx(MSG_NULL_TRACKER_RESPONSE);
  }
  const bencode::BDE& failure = dict[BtAnnounce::FAILURE_REASON];
  if(failure.isString()) {
    throw DlAbortEx
      (StringFormat(EX_TRACKER_FAILURE, failure.s().c_str()).str());
  }
  const bencode::BDE& warn = dict[BtAnnounce::WARNING_MESSAGE];
  if(warn.isString()) {
    logger->warn(MSG_TRACKER_WARNING_MESSAGE, warn.s().c_str());
  }
  const bencode::BDE& tid = dict[BtAnnounce::TRACKER_ID];
  if(tid.isString()) {
    trackerId = tid.s();
    logger->debug("Tracker ID:%s", trackerId.c_str());
  }
  const bencode::BDE& ival = dict[BtAnnounce::INTERVAL];
  if(ival.isInteger() && ival.i() > 0) {
    interval = ival.i();
    logger->debug("Interval:%d", interval);
  }
  const bencode::BDE& mival = dict[BtAnnounce::MIN_INTERVAL];
  if(mival.isInteger() && mival.i() > 0) {
    minInterval = mival.i();
    logger->debug("Min interval:%d", minInterval);
    if(minInterval > interval) {
      minInterval = interval;
    }
  } else {
    // Use interval as a minInterval if minInterval is not supplied.
    minInterval = interval;
  }
  const bencode::BDE& comp = dict[BtAnnounce::COMPLETE];
  if(comp.isInteger()) {
    complete = comp.i();
    logger->debug("Complete:%d", complete);
  }
  const bencode::BDE& incomp = dict[BtAnnounce::INCOMPLETE];
  if(incomp.isInteger()) {
    incomplete = incomp.i();
    logger->debug("Incomplete:%d", incomplete);
  }
  const bencode::BDE& peerData = dict[BtAnnounce::PEERS];
  if(!peerData.isNone()) {
    if(!btRuntime->isHalt() && btRuntime->lessThanMinPeers()) {
      DelegatingPeerListProcessor proc;
      std::deque<SharedHandle<Peer> > peers;
      proc.extractPeer(peers, peerData);
      peerStorage->addPeer(peers);
    }
  } else {
    logger->info(MSG_NO_PEER_LIST_RECEIVED);
  }
}

bool DefaultBtAnnounce::noMoreAnnounce() {
  return (trackers == 0 &&
	  btRuntime->isHalt() &&
	  !announceList.countStoppedAllowedTier());
}

void DefaultBtAnnounce::shuffleAnnounce() {
  announceList.shuffle();
}

void DefaultBtAnnounce::setRandomizer(const RandomizerHandle& randomizer)
{
  _randomizer = randomizer;
}

void DefaultBtAnnounce::setBtRuntime(const BtRuntimeHandle& btRuntime)
{
  this->btRuntime = btRuntime;
}

BtRuntimeHandle DefaultBtAnnounce::getBtRuntime() const
{
  return btRuntime;
}

void DefaultBtAnnounce::setPieceStorage(const PieceStorageHandle& pieceStorage)
{
  this->pieceStorage = pieceStorage;
}

PieceStorageHandle DefaultBtAnnounce::getPieceStorage() const
{
  return pieceStorage;
}

void DefaultBtAnnounce::setPeerStorage(const PeerStorageHandle& peerStorage)
{
  this->peerStorage = peerStorage;
}

PeerStorageHandle DefaultBtAnnounce::getPeerStorage() const
{
  return peerStorage;
}

void DefaultBtAnnounce::overrideMinInterval(time_t interval)
{
  minInterval = interval;
}

} // namespace aria2
