/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <iostream>
#include <map>

#include "nspr.h"
#include "nss.h"
#include "ssl.h"

#include "mozilla/RefPtr.h"
#include "mozilla/Tuple.h"

#define GTEST_HAS_RTTI 0
#include "gtest/gtest.h"

#include "signaling/src/sdp/SdpMediaSection.h"
#include "signaling/src/sdp/SipccSdpParser.h"
#include "signaling/src/jsep/JsepCodecDescription.h"
#include "signaling/src/jsep/JsepTrack.h"
#include "signaling/src/jsep/JsepSession.h"
#include "signaling/src/jsep/JsepSessionImpl.h"
#include "signaling/src/jsep/JsepTrack.h"

namespace mozilla {
static std::string kAEqualsCandidate("a=candidate:");
const static size_t kNumCandidatesPerComponent = 3;

class JsepSessionTestBase : public ::testing::Test
{
public:
  static void SetUpTestCase() {
    NSS_NoDB_Init(nullptr);
    NSS_SetDomesticPolicy();
  }
};

class FakeUuidGenerator : public mozilla::JsepUuidGenerator
{
public:
  bool
  Generate(std::string* str)
  {
    std::ostringstream os;
    os << "FAKE_UUID_" << ++ctr;
    *str = os.str();

    return true;
  }

private:
  static uint64_t ctr;
};

uint64_t FakeUuidGenerator::ctr = 1000;

class JsepSessionTest : public JsepSessionTestBase,
                        public ::testing::WithParamInterface<std::string>
{
public:
  JsepSessionTest()
      : mSdpHelper(&mLastError)
  {
    mSessionOff = MakeUnique<JsepSessionImpl>("Offerer", MakeUnique<FakeUuidGenerator>());
    mSessionAns = MakeUnique<JsepSessionImpl>("Answerer", MakeUnique<FakeUuidGenerator>());

    EXPECT_EQ(NS_OK, mSessionOff->Init());
    EXPECT_EQ(NS_OK, mSessionAns->Init());

    mOffererTransport = MakeUnique<TransportData>();
    mAnswererTransport = MakeUnique<TransportData>();

    AddTransportData(*mSessionOff, *mOffererTransport);
    AddTransportData(*mSessionAns, *mAnswererTransport);

    mOffCandidates = MakeUnique<CandidateSet>();
    mAnsCandidates = MakeUnique<CandidateSet>();
  }
protected:
  struct TransportData {
    std::string mIceUfrag;
    std::string mIcePwd;
    std::map<std::string, std::vector<uint8_t> > mFingerprints;
  };

  void
  AddDtlsFingerprint(const std::string& alg, JsepSessionImpl& session,
                     TransportData& tdata)
  {
    std::vector<uint8_t> fp;
    fp.assign((alg == "sha-1") ? 20 : 32,
              (session.GetName() == "Offerer") ? 0x4f : 0x41);
    session.AddDtlsFingerprint(alg, fp);
    tdata.mFingerprints[alg] = fp;
  }

  void
  AddTransportData(JsepSessionImpl& session, TransportData& tdata)
  {
    // Values here semi-borrowed from JSEP draft.
    tdata.mIceUfrag = session.GetName() + "-ufrag";
    tdata.mIcePwd = session.GetName() + "-1234567890";
    session.SetIceCredentials(tdata.mIceUfrag, tdata.mIcePwd);
    AddDtlsFingerprint("sha-1", session, tdata);
    AddDtlsFingerprint("sha-256", session, tdata);
  }

  std::string
  CreateOffer(const Maybe<JsepOfferOptions>& options = Nothing())
  {
    JsepOfferOptions defaultOptions;
    const JsepOfferOptions& optionsRef = options ? *options : defaultOptions;
    std::string offer;
    nsresult rv;
    rv = mSessionOff->CreateOffer(optionsRef, &offer);
    EXPECT_EQ(NS_OK, rv) << mSessionOff->GetLastError();

    std::cerr << "OFFER: " << offer << std::endl;

    ValidateTransport(*mOffererTransport, offer);

    return offer;
  }

  void
  AddTracks(JsepSessionImpl& side)
  {
    // Add tracks.
    if (types.empty()) {
      types = BuildTypes(GetParam());
    }
    AddTracks(side, types);

    // Now that we have added streams, we expect audio, then video, then
    // application in the SDP, regardless of the order in which the streams were
    // added.
    std::sort(types.begin(), types.end());
  }

  void
  AddTracks(JsepSessionImpl& side, const std::string& mediatypes)
  {
    AddTracks(side, BuildTypes(mediatypes));
  }

  std::vector<SdpMediaSection::MediaType>
  BuildTypes(const std::string& mediatypes)
  {
    std::vector<SdpMediaSection::MediaType> result;
    size_t ptr = 0;

    for (;;) {
      size_t comma = mediatypes.find(',', ptr);
      std::string chunk = mediatypes.substr(ptr, comma - ptr);

      SdpMediaSection::MediaType type;
      if (chunk == "audio") {
        type = SdpMediaSection::kAudio;
      } else if (chunk == "video") {
        type = SdpMediaSection::kVideo;
      } else if (chunk == "datachannel") {
        type = SdpMediaSection::kApplication;
      } else {
        MOZ_CRASH();
      }
      result.push_back(type);

      if (comma == std::string::npos)
        break;
      ptr = comma + 1;
    }

    return result;
  }

  void
  AddTracks(JsepSessionImpl& side,
            const std::vector<SdpMediaSection::MediaType>& mediatypes)
  {
    FakeUuidGenerator uuid_gen;
    std::string stream_id;
    std::string track_id;

    ASSERT_TRUE(uuid_gen.Generate(&stream_id));

    AddTracksToStream(side, stream_id, mediatypes);
  }

  void
  AddTracksToStream(JsepSessionImpl& side,
                    const std::string stream_id,
                    const std::string& mediatypes)
  {
    AddTracksToStream(side, stream_id, BuildTypes(mediatypes));
  }

  void
  AddTracksToStream(JsepSessionImpl& side,
                    const std::string stream_id,
                    const std::vector<SdpMediaSection::MediaType>& mediatypes)

  {
    FakeUuidGenerator uuid_gen;
    std::string track_id;

    for (auto track = mediatypes.begin(); track != mediatypes.end(); ++track) {
      ASSERT_TRUE(uuid_gen.Generate(&track_id));

      RefPtr<JsepTrack> mst(new JsepTrack(*track, stream_id, track_id));
      side.AddTrack(mst);
    }
  }

  bool HasMediaStream(std::vector<RefPtr<JsepTrack>> tracks) const {
    for (auto i = tracks.begin(); i != tracks.end(); ++i) {
      if ((*i)->GetMediaType() != SdpMediaSection::kApplication) {
        return 1;
      }
    }
    return 0;
  }

  const std::string GetFirstLocalStreamId(JsepSessionImpl& side) const {
    auto tracks = side.GetLocalTracks();
    return (*tracks.begin())->GetStreamId();
  }

  std::vector<std::string>
  GetMediaStreamIds(std::vector<RefPtr<JsepTrack>> tracks) const {
    std::vector<std::string> ids;
    for (auto i = tracks.begin(); i != tracks.end(); ++i) {
      // data channels don't have msid's
      if ((*i)->GetMediaType() == SdpMediaSection::kApplication) {
        continue;
      }
      ids.push_back((*i)->GetStreamId());
    }
    return ids;
  }

  std::vector<std::string>
  GetLocalMediaStreamIds(JsepSessionImpl& side) const {
    return GetMediaStreamIds(side.GetLocalTracks());
  }

  std::vector<std::string>
  GetRemoteMediaStreamIds(JsepSessionImpl& side) const {
    return GetMediaStreamIds(side.GetRemoteTracks());
  }

  std::vector<std::string>
  sortUniqueStrVector(std::vector<std::string> in) const {
    std::sort(in.begin(), in.end());
    auto it = std::unique(in.begin(), in.end());
    in.resize( std::distance(in.begin(), it));
    return in;
  }

  std::vector<std::string>
  GetLocalUniqueStreamIds(JsepSessionImpl& side) const {
    return sortUniqueStrVector(GetLocalMediaStreamIds(side));
  }

  std::vector<std::string>
  GetRemoteUniqueStreamIds(JsepSessionImpl& side) const {
    return sortUniqueStrVector(GetRemoteMediaStreamIds(side));
  }

  RefPtr<JsepTrack> GetTrack(JsepSessionImpl& side,
                             SdpMediaSection::MediaType type,
                             size_t index) const {
    auto tracks = side.GetLocalTracks();

    for (auto i = tracks.begin(); i != tracks.end(); ++i) {
      if ((*i)->GetMediaType() != type) {
        continue;
      }

      if (index != 0) {
        --index;
        continue;
      }

      return *i;
    }

    return RefPtr<JsepTrack>(nullptr);
  }

  RefPtr<JsepTrack> GetTrackOff(size_t index,
                                SdpMediaSection::MediaType type) {
    return GetTrack(*mSessionOff, type, index);
  }

  RefPtr<JsepTrack> GetTrackAns(size_t index,
                                SdpMediaSection::MediaType type) {
    return GetTrack(*mSessionAns, type, index);
  }

  class ComparePairsByLevel {
    public:
      bool operator()(const JsepTrackPair& lhs,
                      const JsepTrackPair& rhs) const {
        return lhs.mLevel < rhs.mLevel;
      }
  };

  std::vector<JsepTrackPair> GetTrackPairsByLevel(JsepSessionImpl& side) const {
    auto pairs = side.GetNegotiatedTrackPairs();
    std::sort(pairs.begin(), pairs.end(), ComparePairsByLevel());
    return pairs;
  }

  bool Equals(const SdpFingerprintAttributeList::Fingerprint& f1,
              const SdpFingerprintAttributeList::Fingerprint& f2) const {
    if (f1.hashFunc != f2.hashFunc) {
      return false;
    }

    if (f1.fingerprint != f2.fingerprint) {
      return false;
    }

    return true;
  }

  bool Equals(const SdpFingerprintAttributeList& f1,
              const SdpFingerprintAttributeList& f2) const {
    if (f1.mFingerprints.size() != f2.mFingerprints.size()) {
      return false;
    }

    for (size_t i=0; i<f1.mFingerprints.size(); ++i) {
      if (!Equals(f1.mFingerprints[i], f2.mFingerprints[i])) {
        return false;
      }
    }

    return true;
  }

  bool Equals(const UniquePtr<JsepDtlsTransport>& t1,
              const UniquePtr<JsepDtlsTransport>& t2) const {
    if (!t1 && !t2) {
      return true;
    }

    if (!t1 || !t2) {
      return false;
    }

    if (!Equals(t1->GetFingerprints(),  t2->GetFingerprints())) {
      return false;
    }

    if (t1->GetRole() != t2->GetRole()) {
      return false;
    }

    return true;
  }


  bool Equals(const UniquePtr<JsepIceTransport>& t1,
              const UniquePtr<JsepIceTransport>& t2) const {
    if (!t1 && !t2) {
      return true;
    }

    if (!t1 || !t2) {
      return false;
    }

    if (t1->GetUfrag() != t2->GetUfrag()) {
      return false;
    }

    if (t1->GetPassword() != t2->GetPassword()) {
      return false;
    }

    return true;
  }

  bool Equals(const RefPtr<JsepTransport>& t1,
              const RefPtr<JsepTransport>& t2) const {
    if (!t1 && !t2) {
      return true;
    }

    if (!t1 || !t2) {
      return false;
    }

    if (t1->mTransportId != t2->mTransportId) {
      return false;
    }

    if (t1->mComponents != t2->mComponents) {
      return false;
    }

    if (!Equals(t1->mIce, t2->mIce)) {
      return false;
    }

    return true;
  }

  bool Equals(const JsepTrackPair& p1,
              const JsepTrackPair& p2) const {
    if (p1.mLevel != p2.mLevel) {
      return false;
    }

    // We don't check things like BundleLevel(), since that can change without
    // any changes to the transport, which is what we're really interested in.

    if (p1.mSending.get() != p2.mSending.get()) {
      return false;
    }

    if (p1.mReceiving.get() != p2.mReceiving.get()) {
      return false;
    }

    if (!Equals(p1.mRtpTransport, p2.mRtpTransport)) {
      return false;
    }

    if (!Equals(p1.mRtcpTransport, p2.mRtcpTransport)) {
      return false;
    }

    return true;
  }

  size_t GetTrackCount(JsepSessionImpl& side,
                       SdpMediaSection::MediaType type) const {
    auto tracks = side.GetLocalTracks();
    size_t result = 0;
    for (auto i = tracks.begin(); i != tracks.end(); ++i) {
      if ((*i)->GetMediaType() == type) {
        ++result;
      }
    }
    return result;
  }

  UniquePtr<Sdp> GetParsedLocalDescription(const JsepSessionImpl& side) const {
    return Parse(side.GetLocalDescription(kJsepDescriptionCurrent));
  }

  SdpMediaSection* GetMsection(Sdp& sdp,
                               SdpMediaSection::MediaType type,
                               size_t index) const {
    for (size_t i = 0; i < sdp.GetMediaSectionCount(); ++i) {
      auto& msection = sdp.GetMediaSection(i);
      if (msection.GetMediaType() != type) {
        continue;
      }

      if (index) {
        --index;
        continue;
      }

      return &msection;
    }

    return nullptr;
  }

  void
  SetPayloadTypeNumber(JsepSession& session,
                       const std::string& codecName,
                       const std::string& payloadType)
  {
    for (auto* codec : session.Codecs()) {
      if (codec->mName == codecName) {
        codec->mDefaultPt = payloadType;
      }
    }
  }

  void
  SetCodecEnabled(JsepSession& session,
                  const std::string& codecName,
                  bool enabled)
  {
    for (auto* codec : session.Codecs()) {
      if (codec->mName == codecName) {
        codec->mEnabled = enabled;
      }
    }
  }

  void
  EnsureNegotiationFailure(SdpMediaSection::MediaType type,
                           const std::string& codecName)
  {
    for (auto i = mSessionOff->Codecs().begin(); i != mSessionOff->Codecs().end();
         ++i) {
      auto* codec = *i;
      if (codec->mType == type && codec->mName != codecName) {
        codec->mEnabled = false;
      }
    }

    for (auto i = mSessionAns->Codecs().begin(); i != mSessionAns->Codecs().end();
         ++i) {
      auto* codec = *i;
      if (codec->mType == type && codec->mName == codecName) {
        codec->mEnabled = false;
      }
    }
  }

  std::string
  CreateAnswer()
  {
    JsepAnswerOptions options;
    std::string answer;
    nsresult rv = mSessionAns->CreateAnswer(options, &answer);
    EXPECT_EQ(NS_OK, rv);

    std::cerr << "ANSWER: " << answer << std::endl;

    ValidateTransport(*mAnswererTransport, answer);

    return answer;
  }

  static const uint32_t NO_CHECKS = 0;
  static const uint32_t CHECK_SUCCESS = 1;
  static const uint32_t CHECK_TRACKS = 1 << 2;
  static const uint32_t ALL_CHECKS = CHECK_SUCCESS | CHECK_TRACKS;

  void OfferAnswer(uint32_t checkFlags = ALL_CHECKS,
                   const Maybe<JsepOfferOptions>& options = Nothing()) {
    std::string offer = CreateOffer(options);
    SetLocalOffer(offer, checkFlags);
    SetRemoteOffer(offer, checkFlags);

    std::string answer = CreateAnswer();
    SetLocalAnswer(answer, checkFlags);
    SetRemoteAnswer(answer, checkFlags);
  }

  void
  SetLocalOffer(const std::string& offer, uint32_t checkFlags = ALL_CHECKS)
  {
    nsresult rv = mSessionOff->SetLocalDescription(kJsepSdpOffer, offer);

    if (checkFlags & CHECK_SUCCESS) {
      ASSERT_EQ(NS_OK, rv);
    }

    if (checkFlags & CHECK_TRACKS) {
      // Check that the transports exist.
      ASSERT_EQ(types.size(), mSessionOff->GetTransports().size());
      auto tracks = mSessionOff->GetLocalTracks();
      for (size_t i = 0; i < types.size(); ++i) {
        ASSERT_NE("", tracks[i]->GetStreamId());
        ASSERT_NE("", tracks[i]->GetTrackId());
        if (tracks[i]->GetMediaType() != SdpMediaSection::kApplication) {
          std::string msidAttr("a=msid:");
          msidAttr += tracks[i]->GetStreamId();
          msidAttr += " ";
          msidAttr += tracks[i]->GetTrackId();
          ASSERT_NE(std::string::npos, offer.find(msidAttr))
            << "Did not find " << msidAttr << " in offer";
        }
      }
      if (types.size() == 1 &&
          tracks[0]->GetMediaType() == SdpMediaSection::kApplication) {
        ASSERT_EQ(std::string::npos, offer.find("a=ssrc"))
          << "Data channel should not contain SSRC";
      }
    }
  }

  void
  SetRemoteOffer(const std::string& offer, uint32_t checkFlags = ALL_CHECKS)
  {
    nsresult rv = mSessionAns->SetRemoteDescription(kJsepSdpOffer, offer);

    if (checkFlags & CHECK_SUCCESS) {
      ASSERT_EQ(NS_OK, rv);
    }

    if (checkFlags & CHECK_TRACKS) {
      auto tracks = mSessionAns->GetRemoteTracks();
      // Now verify that the right stuff is in the tracks.
      ASSERT_EQ(types.size(), tracks.size());
      for (size_t i = 0; i < tracks.size(); ++i) {
        ASSERT_EQ(types[i], tracks[i]->GetMediaType());
        ASSERT_NE("", tracks[i]->GetStreamId());
        ASSERT_NE("", tracks[i]->GetTrackId());
        if (tracks[i]->GetMediaType() != SdpMediaSection::kApplication) {
          std::string msidAttr("a=msid:");
          msidAttr += tracks[i]->GetStreamId();
          msidAttr += " ";
          msidAttr += tracks[i]->GetTrackId();
          ASSERT_NE(std::string::npos, offer.find(msidAttr))
            << "Did not find " << msidAttr << " in offer";
        }
      }
    }
  }

  void
  SetLocalAnswer(const std::string& answer, uint32_t checkFlags = ALL_CHECKS)
  {
    nsresult rv = mSessionAns->SetLocalDescription(kJsepSdpAnswer, answer);
    if (checkFlags & CHECK_SUCCESS) {
      ASSERT_EQ(NS_OK, rv);
    }

    if (checkFlags & CHECK_TRACKS) {
      // Verify that the right stuff is in the tracks.
      auto pairs = mSessionAns->GetNegotiatedTrackPairs();
      ASSERT_EQ(types.size(), pairs.size());
      for (size_t i = 0; i < types.size(); ++i) {
        ASSERT_TRUE(pairs[i].mSending);
        ASSERT_EQ(types[i], pairs[i].mSending->GetMediaType());
        ASSERT_TRUE(pairs[i].mReceiving);
        ASSERT_EQ(types[i], pairs[i].mReceiving->GetMediaType());
        ASSERT_NE("", pairs[i].mSending->GetStreamId());
        ASSERT_NE("", pairs[i].mSending->GetTrackId());
        // These might have been in the SDP, or might have been randomly
        // chosen by JsepSessionImpl
        ASSERT_NE("", pairs[i].mReceiving->GetStreamId());
        ASSERT_NE("", pairs[i].mReceiving->GetTrackId());

        if (pairs[i].mReceiving->GetMediaType() != SdpMediaSection::kApplication) {
          std::string msidAttr("a=msid:");
          msidAttr += pairs[i].mSending->GetStreamId();
          msidAttr += " ";
          msidAttr += pairs[i].mSending->GetTrackId();
          ASSERT_NE(std::string::npos, answer.find(msidAttr))
            << "Did not find " << msidAttr << " in offer";
        }
      }
      if (types.size() == 1 &&
          pairs[0].mReceiving->GetMediaType() == SdpMediaSection::kApplication) {
        ASSERT_EQ(std::string::npos, answer.find("a=ssrc"))
          << "Data channel should not contain SSRC";
      }
    }
    std::cerr << "OFFER pairs:" << std::endl;
    DumpTrackPairs(*mSessionOff);
  }

  void
  SetRemoteAnswer(const std::string& answer, uint32_t checkFlags = ALL_CHECKS)
  {
    nsresult rv = mSessionOff->SetRemoteDescription(kJsepSdpAnswer, answer);
    if (checkFlags & CHECK_SUCCESS) {
      ASSERT_EQ(NS_OK, rv);
    }

    if (checkFlags & CHECK_TRACKS) {
      // Verify that the right stuff is in the tracks.
      auto pairs = mSessionOff->GetNegotiatedTrackPairs();
      ASSERT_EQ(types.size(), pairs.size());
      for (size_t i = 0; i < types.size(); ++i) {
        ASSERT_TRUE(pairs[i].mSending);
        ASSERT_EQ(types[i], pairs[i].mSending->GetMediaType());
        ASSERT_TRUE(pairs[i].mReceiving);
        ASSERT_EQ(types[i], pairs[i].mReceiving->GetMediaType());
        ASSERT_NE("", pairs[i].mSending->GetStreamId());
        ASSERT_NE("", pairs[i].mSending->GetTrackId());
        // These might have been in the SDP, or might have been randomly
        // chosen by JsepSessionImpl
        ASSERT_NE("", pairs[i].mReceiving->GetStreamId());
        ASSERT_NE("", pairs[i].mReceiving->GetTrackId());

        if (pairs[i].mReceiving->GetMediaType() != SdpMediaSection::kApplication) {
          std::string msidAttr("a=msid:");
          msidAttr += pairs[i].mReceiving->GetStreamId();
          msidAttr += " ";
          msidAttr += pairs[i].mReceiving->GetTrackId();
          ASSERT_NE(std::string::npos, answer.find(msidAttr))
            << "Did not find " << msidAttr << " in answer";
        }
      }
    }
    std::cerr << "ANSWER pairs:" << std::endl;
    DumpTrackPairs(*mSessionAns);
  }

  typedef enum {
    RTP = 1,
    RTCP = 2
  } ComponentType;

  class CandidateSet {
    public:
      CandidateSet() {}

      void Gather(JsepSession& session,
                  const std::vector<SdpMediaSection::MediaType>& types,
                  ComponentType maxComponent = RTCP)
      {
        for (size_t level = 0; level < types.size(); ++level) {
          Gather(session, level, RTP);
          if (types[level] != SdpMediaSection::kApplication &&
              maxComponent == RTCP) {
            Gather(session, level, RTCP);
          }
        }
        FinishGathering(session);
      }

      void Gather(JsepSession& session, size_t level, ComponentType component)
      {
        static uint16_t port = 1000;
        std::vector<std::string> candidates;
        for (size_t i = 0; i < kNumCandidatesPerComponent; ++i) {
          ++port;
          std::ostringstream candidate;
          candidate << "0 " << static_cast<uint16_t>(component)
                    << " UDP 9999 192.168.0.1 " << port << " typ host";
          std::string mid;
          bool skipped;
          session.AddLocalIceCandidate(kAEqualsCandidate + candidate.str(),
                                       level, &mid, &skipped);
          if (!skipped) {
            mCandidatesToTrickle.push_back(
                Tuple<Level, Mid, Candidate>(
                  level, mid, kAEqualsCandidate + candidate.str()));
            candidates.push_back(candidate.str());
          }
        }

        // Stomp existing candidates
        mCandidates[level][component] = candidates;

        // Stomp existing defaults
        mDefaultCandidates[level][component] =
          std::make_pair("192.168.0.1", port);
        session.UpdateDefaultCandidate(
            mDefaultCandidates[level][RTP].first,
            mDefaultCandidates[level][RTP].second,
            // Will be empty string if not present, which is how we indicate
            // that there is no default for RTCP
            mDefaultCandidates[level][RTCP].first,
            mDefaultCandidates[level][RTCP].second,
            level);
      }

      void FinishGathering(JsepSession& session) const
      {
        // Copy so we can be terse and use []
        for (auto levelAndCandidates : mDefaultCandidates) {
          ASSERT_EQ(1U, levelAndCandidates.second.count(RTP));
          // do a final UpdateDefaultCandidate here in case candidates were
          // cleared during renegotiation.
          session.UpdateDefaultCandidate(
              levelAndCandidates.second[RTP].first,
              levelAndCandidates.second[RTP].second,
              // Will be empty string if not present, which is how we indicate
              // that there is no default for RTCP
              levelAndCandidates.second[RTCP].first,
              levelAndCandidates.second[RTCP].second,
              levelAndCandidates.first);
          session.EndOfLocalCandidates(levelAndCandidates.first);
        }
      }

      void Trickle(JsepSession& session)
      {
        for (const auto& levelMidAndCandidate : mCandidatesToTrickle) {
          Level level;
          Mid mid;
          Candidate candidate;
          Tie(level, mid, candidate) = levelMidAndCandidate;
  std::cerr << "trickeling candidate: " << candidate << " level: " << level << " mid: " << mid << std::endl;
          session.AddRemoteIceCandidate(candidate, mid, level);
        }
        mCandidatesToTrickle.clear();
      }

      void CheckRtpCandidates(bool expectRtpCandidates,
                              const SdpMediaSection& msection,
                              size_t transportLevel,
                              const std::string& context) const
      {
        auto& attrs = msection.GetAttributeList();

        ASSERT_EQ(expectRtpCandidates,
                  attrs.HasAttribute(SdpAttribute::kCandidateAttribute))
          << context << " (level " << msection.GetLevel() << ")";

        if (expectRtpCandidates) {
          // Copy so we can be terse and use []
          auto expectedCandidates = mCandidates;
          ASSERT_LE(kNumCandidatesPerComponent,
                    expectedCandidates[transportLevel][RTP].size());

          auto& candidates = attrs.GetCandidate();
          ASSERT_LE(kNumCandidatesPerComponent, candidates.size())
            << context << " (level " << msection.GetLevel() << ")";
          for (size_t i = 0; i < kNumCandidatesPerComponent; ++i) {
            ASSERT_EQ(expectedCandidates[transportLevel][RTP][i], candidates[i])
              << context << " (level " << msection.GetLevel() << ")";
          }
        }
      }

      void CheckRtcpCandidates(bool expectRtcpCandidates,
                               const SdpMediaSection& msection,
                               size_t transportLevel,
                               const std::string& context) const
      {
        auto& attrs = msection.GetAttributeList();

        if (expectRtcpCandidates) {
          // Copy so we can be terse and use []
          auto expectedCandidates = mCandidates;
          ASSERT_LE(kNumCandidatesPerComponent,
                    expectedCandidates[transportLevel][RTCP].size());

          ASSERT_TRUE(attrs.HasAttribute(SdpAttribute::kCandidateAttribute))
            << context << " (level " << msection.GetLevel() << ")";
          auto& candidates = attrs.GetCandidate();
          ASSERT_EQ(kNumCandidatesPerComponent * 2, candidates.size())
            << context << " (level " << msection.GetLevel() << ")";
          for (size_t i = 0; i < kNumCandidatesPerComponent; ++i) {
            ASSERT_EQ(expectedCandidates[transportLevel][RTCP][i],
                      candidates[i + kNumCandidatesPerComponent])
              << context << " (level " << msection.GetLevel() << ")";
          }
        }
      }

      void CheckDefaultRtpCandidate(bool expectDefault,
                                    const SdpMediaSection& msection,
                                    size_t transportLevel,
                                    const std::string& context) const
      {
        Address expectedAddress = "0.0.0.0";
        Port expectedPort = 9U;

        if (expectDefault) {
          // Copy so we can be terse and use []
          auto defaultCandidates = mDefaultCandidates;
          expectedAddress = defaultCandidates[transportLevel][RTP].first;
          expectedPort = defaultCandidates[transportLevel][RTP].second;
        }

        // if bundle-only attribute is present, expect port 0
        const SdpAttributeList& attrs = msection.GetAttributeList();
        if (attrs.HasAttribute(SdpAttribute::kBundleOnlyAttribute)) {
          expectedPort = 0U;
        }

        ASSERT_EQ(expectedAddress, msection.GetConnection().GetAddress())
          << context << " (level " << msection.GetLevel() << ")";
        ASSERT_EQ(expectedPort, msection.GetPort())
          << context << " (level " << msection.GetLevel() << ")";
      }

      void CheckDefaultRtcpCandidate(bool expectDefault,
                                     const SdpMediaSection& msection,
                                     size_t transportLevel,
                                     const std::string& context) const
      {
        if (expectDefault) {
          // Copy so we can be terse and use []
          auto defaultCandidates = mDefaultCandidates;
          ASSERT_TRUE(msection.GetAttributeList().HasAttribute(
                SdpAttribute::kRtcpAttribute))
            << context << " (level " << msection.GetLevel() << ")";
          auto& rtcpAttr = msection.GetAttributeList().GetRtcp();
          ASSERT_EQ(defaultCandidates[transportLevel][RTCP].second,
                    rtcpAttr.mPort)
            << context << " (level " << msection.GetLevel() << ")";
          ASSERT_EQ(sdp::kInternet, rtcpAttr.mNetType)
            << context << " (level " << msection.GetLevel() << ")";
          ASSERT_EQ(sdp::kIPv4, rtcpAttr.mAddrType)
            << context << " (level " << msection.GetLevel() << ")";
          ASSERT_EQ(defaultCandidates[transportLevel][RTCP].first,
                    rtcpAttr.mAddress)
            << context << " (level " << msection.GetLevel() << ")";
        } else {
          ASSERT_FALSE(msection.GetAttributeList().HasAttribute(
                SdpAttribute::kRtcpAttribute))
            << context << " (level " << msection.GetLevel() << ")";
        }
      }

    private:
      typedef size_t Level;
      typedef std::string Mid;
      typedef std::string Candidate;
      typedef std::string Address;
      typedef uint16_t Port;
      // Default candidates are put into the m-line, c-line, and rtcp
      // attribute for endpoints that don't support ICE.
      std::map<Level,
               std::map<ComponentType,
                        std::pair<Address, Port>>> mDefaultCandidates;
      std::map<Level,
               std::map<ComponentType,
                        std::vector<Candidate>>> mCandidates;
      // Level/mid/candidate tuples that need to be trickled
      std::vector<Tuple<Level, Mid, Candidate>> mCandidatesToTrickle;
  };

  // For streaming parse errors
  std::string
  GetParseErrors(const SipccSdpParser& parser) const
  {
    std::stringstream output;
    for (auto e = parser.GetParseErrors().begin();
         e != parser.GetParseErrors().end();
         ++e) {
      output << e->first << ": " << e->second << std::endl;
    }
    return output.str();
  }

  void CheckEndOfCandidates(bool expectEoc,
                            const SdpMediaSection& msection,
                            const std::string& context)
  {
    if (expectEoc) {
      ASSERT_TRUE(msection.GetAttributeList().HasAttribute(
            SdpAttribute::kEndOfCandidatesAttribute))
        << context << " (level " << msection.GetLevel() << ")";
    } else {
      ASSERT_FALSE(msection.GetAttributeList().HasAttribute(
            SdpAttribute::kEndOfCandidatesAttribute))
        << context << " (level " << msection.GetLevel() << ")";
    }
  }

  void CheckPairs(const JsepSession& session, const std::string& context)
  {
    auto pairs = session.GetNegotiatedTrackPairs();

    for (JsepTrackPair& pair : pairs) {
      ASSERT_TRUE(pair.HasBundleLevel()) << context;
      ASSERT_EQ(0U, pair.BundleLevel()) << context;
    }
  }

  void
  DisableMsid(std::string* sdp) const {
    size_t pos = sdp->find("a=msid-semantic");
    ASSERT_NE(std::string::npos, pos);
    (*sdp)[pos + 2] = 'X'; // garble, a=Xsid-semantic
  }

  void
  DisableBundle(std::string* sdp) const {
    size_t pos = sdp->find("a=group:BUNDLE");
    ASSERT_NE(std::string::npos, pos);
    (*sdp)[pos + 11] = 'G'; // garble, a=group:BUNGLE
  }

  void
  DisableMsection(std::string* sdp, size_t level) const {
    UniquePtr<Sdp> parsed(Parse(*sdp));
    ASSERT_TRUE(parsed.get());
    ASSERT_LT(level, parsed->GetMediaSectionCount());
    SdpHelper::DisableMsection(parsed.get(), &parsed->GetMediaSection(level));
    (*sdp) = parsed->ToString();
  }

  void
  CopyTransportAttributes(std::string* sdp, size_t src_level, size_t dst_level)
  {
    UniquePtr<Sdp> parsed(Parse(*sdp));
    ASSERT_TRUE(parsed.get());
    ASSERT_LT(src_level, parsed->GetMediaSectionCount());
    ASSERT_LT(dst_level, parsed->GetMediaSectionCount());
    nsresult rv = mSdpHelper.CopyTransportParams(2,
                                   parsed->GetMediaSection(src_level),
                                   &parsed->GetMediaSection(dst_level));
    ASSERT_EQ(NS_OK, rv);
    (*sdp) = parsed->ToString();
  }

  void
  ReplaceInSdp(std::string* sdp,
               const char* searchStr,
               const char* replaceStr) const
  {
    if (searchStr[0] == '\0') return;
    size_t pos;
    while ((pos = sdp->find(searchStr)) != std::string::npos) {
      sdp->replace(pos, strlen(searchStr), replaceStr);
    }
  }

  void
  ValidateDisabledMSection(const SdpMediaSection* msection)
  {
    ASSERT_EQ(1U, msection->GetFormats().size());

    auto& attrs = msection->GetAttributeList();
    ASSERT_TRUE(attrs.HasAttribute(SdpAttribute::kMidAttribute));
    ASSERT_TRUE(attrs.HasAttribute(SdpAttribute::kDirectionAttribute));
    ASSERT_FALSE(attrs.HasAttribute(SdpAttribute::kBundleOnlyAttribute));
    ASSERT_EQ(SdpDirectionAttribute::kInactive,
              msection->GetDirectionAttribute().mValue);
    ASSERT_EQ(3U, attrs.Count());
    if (msection->GetMediaType() == SdpMediaSection::kAudio) {
      ASSERT_EQ("0", msection->GetFormats()[0]);
      const SdpRtpmapAttributeList::Rtpmap* rtpmap(msection->FindRtpmap("0"));
      ASSERT_TRUE(rtpmap);
      ASSERT_EQ("0", rtpmap->pt);
      ASSERT_EQ("PCMU", rtpmap->name);
    } else if (msection->GetMediaType() == SdpMediaSection::kVideo) {
      ASSERT_EQ("120", msection->GetFormats()[0]);
      const SdpRtpmapAttributeList::Rtpmap* rtpmap(msection->FindRtpmap("120"));
      ASSERT_TRUE(rtpmap);
      ASSERT_EQ("120", rtpmap->pt);
      ASSERT_EQ("VP8", rtpmap->name);
    } else if (msection->GetMediaType() == SdpMediaSection::kApplication) {
      ASSERT_EQ("0", msection->GetFormats()[0]);
      const SdpSctpmapAttributeList::Sctpmap* sctpmap(msection->GetSctpmap());
      ASSERT_TRUE(sctpmap);
      ASSERT_EQ("0", sctpmap->pt);
      ASSERT_EQ("rejected", sctpmap->name);
      ASSERT_EQ(0U, sctpmap->streams);
    } else {
      // Not that we would have any test which tests this...
      ASSERT_EQ("19", msection->GetFormats()[0]);
      const SdpRtpmapAttributeList::Rtpmap* rtpmap(msection->FindRtpmap("19"));
      ASSERT_TRUE(rtpmap);
      ASSERT_EQ("19", rtpmap->pt);
      ASSERT_EQ("reserved", rtpmap->name);
    }
  }

  void
  ValidateSetupAttribute(const JsepSessionImpl& side,
                         const SdpSetupAttribute::Role expectedRole)
  {
    auto sdp = GetParsedLocalDescription(side);
    for (size_t i = 0; sdp && i < sdp->GetMediaSectionCount(); ++i) {
      if (sdp->GetMediaSection(i).GetAttributeList().HasAttribute(
            SdpAttribute::kSetupAttribute)) {
        auto role = sdp->GetMediaSection(i).GetAttributeList().GetSetup().mRole;
        ASSERT_EQ(expectedRole, role);
      }
    }
  }

  void
  DumpTrack(const JsepTrack& track)
  {
    const JsepTrackNegotiatedDetails* details = track.GetNegotiatedDetails();
    std::cerr << "  type=" << track.GetMediaType() << std::endl;
    std::cerr << "  encodings=" << std::endl;
    for (size_t i = 0; i < details->GetEncodingCount(); ++i) {
      const JsepTrackEncoding& encoding = details->GetEncoding(i);
      std::cerr << "    id=" << encoding.mRid << std::endl;
      for (const JsepCodecDescription* codec : encoding.GetCodecs()) {
        std::cerr << "      " << codec->mName
                  << " enabled(" << (codec->mEnabled?"yes":"no") << ")";
        if (track.GetMediaType() == SdpMediaSection::kAudio) {
          const JsepAudioCodecDescription* audioCodec =
              static_cast<const JsepAudioCodecDescription*>(codec);
          std::cerr << " dtmf(" << (audioCodec->mDtmfEnabled?"yes":"no") << ")";
        }
        std::cerr << std::endl;
      }
    }
  }

  void
  DumpTrackPairs(const JsepSessionImpl& session)
  {
    auto pairs = mSessionAns->GetNegotiatedTrackPairs();
    for (auto i = pairs.begin(); i != pairs.end(); ++i) {
      std::cerr << "Track pair " << i->mLevel << std::endl;
      if (i->mSending) {
        std::cerr << "Sending-->" << std::endl;
        DumpTrack(*i->mSending);
      }
      if (i->mReceiving) {
        std::cerr << "Receiving-->" << std::endl;
        DumpTrack(*i->mReceiving);
      }
    }
  }

  UniquePtr<Sdp>
  Parse(const std::string& sdp) const
  {
    SipccSdpParser parser;
    UniquePtr<Sdp> parsed = parser.Parse(sdp);
    EXPECT_TRUE(parsed.get()) << "Should have valid SDP" << std::endl
                              << "Errors were: " << GetParseErrors(parser);
    return parsed;
  }

  void
  SwapOfferAnswerRoles()
  {
    mSessionOff.swap(mSessionAns);
    mOffCandidates.swap(mAnsCandidates);
    mOffererTransport.swap(mAnswererTransport);
  }

  UniquePtr<JsepSessionImpl> mSessionOff;
  UniquePtr<CandidateSet> mOffCandidates;
  UniquePtr<JsepSessionImpl> mSessionAns;
  UniquePtr<CandidateSet> mAnsCandidates;

  std::vector<SdpMediaSection::MediaType> types;
  std::vector<std::pair<std::string, uint16_t>> mGatheredCandidates;

private:
  void
  ValidateTransport(TransportData& source, const std::string& sdp_str)
  {
    UniquePtr<Sdp> sdp(Parse(sdp_str));
    ASSERT_TRUE(!!sdp);
    size_t num_m_sections = sdp->GetMediaSectionCount();
    for (size_t i = 0; i < num_m_sections; ++i) {
      auto& msection = sdp->GetMediaSection(i);

      if (msection.GetMediaType() == SdpMediaSection::kApplication) {
        ASSERT_EQ(SdpMediaSection::kDtlsSctp, msection.GetProtocol());
      } else {
        ASSERT_EQ(SdpMediaSection::kUdpTlsRtpSavpf, msection.GetProtocol());
      }

      const SdpAttributeList& attrs = msection.GetAttributeList();
      bool bundle_only = attrs.HasAttribute(SdpAttribute::kBundleOnlyAttribute);

      // port 0 only means disabled when the bundle-only attribute is missing
      if (!bundle_only && msection.GetPort() == 0) {
        ValidateDisabledMSection(&msection);
        continue;
      }
      if (!mSdpHelper.IsBundleSlave(*sdp, i)) {
        const SdpAttributeList& attrs = msection.GetAttributeList();

        ASSERT_EQ(source.mIceUfrag, attrs.GetIceUfrag());
        ASSERT_EQ(source.mIcePwd, attrs.GetIcePwd());
        const SdpFingerprintAttributeList& fps = attrs.GetFingerprint();
        for (auto fp = fps.mFingerprints.begin(); fp != fps.mFingerprints.end();
             ++fp) {
          std::string alg_str = "None";

          if (fp->hashFunc == SdpFingerprintAttributeList::kSha1) {
            alg_str = "sha-1";
          } else if (fp->hashFunc == SdpFingerprintAttributeList::kSha256) {
            alg_str = "sha-256";
          }
          ASSERT_EQ(source.mFingerprints[alg_str], fp->fingerprint);
        }

        ASSERT_EQ(source.mFingerprints.size(), fps.mFingerprints.size());
      }
    }
  }

  std::string mLastError;
  SdpHelper mSdpHelper;

  UniquePtr<TransportData> mOffererTransport;
  UniquePtr<TransportData> mAnswererTransport;
};

TEST_F(JsepSessionTestBase, CreateDestroy) {}

TEST_P(JsepSessionTest, CreateOffer)
{
  AddTracks(*mSessionOff);
  CreateOffer();
}

TEST_P(JsepSessionTest, CreateOfferSetLocal)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
}

TEST_P(JsepSessionTest, CreateOfferSetLocalSetRemote)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
}

TEST_P(JsepSessionTest, CreateOfferSetLocalSetRemoteCreateAnswer)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
}

TEST_P(JsepSessionTest, CreateOfferSetLocalSetRemoteCreateAnswerSetLocal)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
}

TEST_P(JsepSessionTest, FullCall)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);
}

TEST_P(JsepSessionTest, GetDescriptions)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  std::string desc = mSessionOff->GetLocalDescription(kJsepDescriptionCurrent);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionOff->GetLocalDescription(kJsepDescriptionPending);
  ASSERT_NE(0U, desc.size());
  desc = mSessionOff->GetLocalDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionOff->GetRemoteDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionAns->GetLocalDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionAns->GetRemoteDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_EQ(0U, desc.size());

  SetRemoteOffer(offer);
  desc = mSessionAns->GetRemoteDescription(kJsepDescriptionCurrent);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionAns->GetRemoteDescription(kJsepDescriptionPending);
  ASSERT_NE(0U, desc.size());
  desc = mSessionAns->GetRemoteDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionAns->GetLocalDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionOff->GetLocalDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionOff->GetRemoteDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_EQ(0U, desc.size());

  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  desc = mSessionAns->GetLocalDescription(kJsepDescriptionCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionAns->GetLocalDescription(kJsepDescriptionPending);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionAns->GetLocalDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionAns->GetRemoteDescription(kJsepDescriptionCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionAns->GetRemoteDescription(kJsepDescriptionPending);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionAns->GetRemoteDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionOff->GetLocalDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionOff->GetRemoteDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_EQ(0U, desc.size());

  SetRemoteAnswer(answer);
  desc = mSessionOff->GetLocalDescription(kJsepDescriptionCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionOff->GetLocalDescription(kJsepDescriptionPending);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionOff->GetLocalDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionOff->GetRemoteDescription(kJsepDescriptionCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionOff->GetRemoteDescription(kJsepDescriptionPending);
  ASSERT_EQ(0U, desc.size());
  desc = mSessionOff->GetRemoteDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionAns->GetLocalDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
  desc = mSessionAns->GetRemoteDescription(kJsepDescriptionPendingOrCurrent);
  ASSERT_NE(0U, desc.size());
}


TEST_P(JsepSessionTest, RenegotiationNoChange)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(types.size(), added.size());
  ASSERT_EQ(0U, removed.size());

  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(types.size(), added.size());
  ASSERT_EQ(0U, removed.size());

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  std::string reoffer = CreateOffer();
  SetLocalOffer(reoffer);
  SetRemoteOffer(reoffer);

  added = mSessionAns->GetRemoteTracksAdded();
  removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  std::string reanswer = CreateAnswer();
  SetLocalAnswer(reanswer);
  SetRemoteAnswer(reanswer);

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }

  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size());
  for (size_t i = 0; i < answererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newAnswererPairs[i]));
  }
}

// Disabled: See Bug 1329028
TEST_P(JsepSessionTest, DISABLED_RenegotiationSwappedRolesNoChange)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(types.size(), added.size());
  ASSERT_EQ(0U, removed.size());

  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(types.size(), added.size());
  ASSERT_EQ(0U, removed.size());

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  SwapOfferAnswerRoles();

  std::string reoffer = CreateOffer();
  SetLocalOffer(reoffer);
  SetRemoteOffer(reoffer);

  added = mSessionAns->GetRemoteTracksAdded();
  removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  std::string reanswer = CreateAnswer();
  SetLocalAnswer(reanswer);
  SetRemoteAnswer(reanswer);

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kPassive);

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size(), newAnswererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newAnswererPairs[i]));
  }

  ASSERT_EQ(answererPairs.size(), newOffererPairs.size());
  for (size_t i = 0; i < answererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newOffererPairs[i]));
  }
}


TEST_P(JsepSessionTest, RenegotiationOffererAddsTrack)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  OfferAnswer();

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  std::vector<SdpMediaSection::MediaType> extraTypes;
  extraTypes.push_back(SdpMediaSection::kAudio);
  extraTypes.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, extraTypes);
  types.insert(types.end(), extraTypes.begin(), extraTypes.end());

  OfferAnswer(CHECK_SUCCESS);

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(2U, added.size());
  ASSERT_EQ(0U, removed.size());
  ASSERT_EQ(SdpMediaSection::kAudio, added[0]->GetMediaType());
  ASSERT_EQ(SdpMediaSection::kVideo, added[1]->GetMediaType());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size() + 2, newOffererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }

  ASSERT_EQ(answererPairs.size() + 2, newAnswererPairs.size());
  for (size_t i = 0; i < answererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newAnswererPairs[i]));
  }
}

TEST_P(JsepSessionTest, RenegotiationAnswererAddsTrack)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  OfferAnswer();

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  std::vector<SdpMediaSection::MediaType> extraTypes;
  extraTypes.push_back(SdpMediaSection::kAudio);
  extraTypes.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionAns, extraTypes);
  types.insert(types.end(), extraTypes.begin(), extraTypes.end());

  // We need to add a recvonly m-section to the offer for this to work
  JsepOfferOptions options;
  options.mOfferToReceiveAudio =
    Some(GetTrackCount(*mSessionOff, SdpMediaSection::kAudio) + 1);
  options.mOfferToReceiveVideo =
    Some(GetTrackCount(*mSessionOff, SdpMediaSection::kVideo) + 1);

  std::string offer = CreateOffer(Some(options));
  SetLocalOffer(offer, CHECK_SUCCESS);
  SetRemoteOffer(offer, CHECK_SUCCESS);

  std::string answer = CreateAnswer();
  SetLocalAnswer(answer, CHECK_SUCCESS);
  SetRemoteAnswer(answer, CHECK_SUCCESS);

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(2U, added.size());
  ASSERT_EQ(0U, removed.size());
  ASSERT_EQ(SdpMediaSection::kAudio, added[0]->GetMediaType());
  ASSERT_EQ(SdpMediaSection::kVideo, added[1]->GetMediaType());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size() + 2, newOffererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }

  ASSERT_EQ(answererPairs.size() + 2, newAnswererPairs.size());
  for (size_t i = 0; i < answererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newAnswererPairs[i]));
  }
}

TEST_P(JsepSessionTest, RenegotiationBothAddTrack)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  OfferAnswer();

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  std::vector<SdpMediaSection::MediaType> extraTypes;
  extraTypes.push_back(SdpMediaSection::kAudio);
  extraTypes.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionAns, extraTypes);
  AddTracks(*mSessionOff, extraTypes);
  types.insert(types.end(), extraTypes.begin(), extraTypes.end());

  OfferAnswer(CHECK_SUCCESS);

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(2U, added.size());
  ASSERT_EQ(0U, removed.size());
  ASSERT_EQ(SdpMediaSection::kAudio, added[0]->GetMediaType());
  ASSERT_EQ(SdpMediaSection::kVideo, added[1]->GetMediaType());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(2U, added.size());
  ASSERT_EQ(0U, removed.size());
  ASSERT_EQ(SdpMediaSection::kAudio, added[0]->GetMediaType());
  ASSERT_EQ(SdpMediaSection::kVideo, added[1]->GetMediaType());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size() + 2, newOffererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }

  ASSERT_EQ(answererPairs.size() + 2, newAnswererPairs.size());
  for (size_t i = 0; i < answererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newAnswererPairs[i]));
  }
}

TEST_P(JsepSessionTest, RenegotiationBothAddTracksToExistingStream)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  if (GetParam() == "datachannel") {
    return;
  }

  OfferAnswer();

  auto oHasStream = HasMediaStream(mSessionOff->GetLocalTracks());
  auto aHasStream = HasMediaStream(mSessionAns->GetLocalTracks());
  ASSERT_EQ(oHasStream, !GetLocalUniqueStreamIds(*mSessionOff).empty());
  ASSERT_EQ(aHasStream, !GetLocalUniqueStreamIds(*mSessionAns).empty());
  ASSERT_EQ(aHasStream, !GetRemoteUniqueStreamIds(*mSessionOff).empty());
  ASSERT_EQ(oHasStream, !GetRemoteUniqueStreamIds(*mSessionAns).empty());

  auto firstOffId = GetFirstLocalStreamId(*mSessionOff);
  auto firstAnsId = GetFirstLocalStreamId(*mSessionAns);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  std::vector<SdpMediaSection::MediaType> extraTypes;
  extraTypes.push_back(SdpMediaSection::kAudio);
  extraTypes.push_back(SdpMediaSection::kVideo);
  AddTracksToStream(*mSessionOff, firstOffId, extraTypes);
  AddTracksToStream(*mSessionAns, firstAnsId, extraTypes);
  types.insert(types.end(), extraTypes.begin(), extraTypes.end());

  OfferAnswer(CHECK_SUCCESS);

  oHasStream = HasMediaStream(mSessionOff->GetLocalTracks());
  aHasStream = HasMediaStream(mSessionAns->GetLocalTracks());

  ASSERT_EQ(oHasStream, !GetLocalUniqueStreamIds(*mSessionOff).empty());
  ASSERT_EQ(aHasStream, !GetLocalUniqueStreamIds(*mSessionAns).empty());
  ASSERT_EQ(aHasStream, !GetRemoteUniqueStreamIds(*mSessionOff).empty());
  ASSERT_EQ(oHasStream, !GetRemoteUniqueStreamIds(*mSessionAns).empty());
  if (oHasStream) {
    ASSERT_STREQ(firstOffId.c_str(),
                 GetFirstLocalStreamId(*mSessionOff).c_str());
  }
  if (aHasStream) {
    ASSERT_STREQ(firstAnsId.c_str(),
                 GetFirstLocalStreamId(*mSessionAns).c_str());

  auto oHasStream = HasMediaStream(mSessionOff->GetLocalTracks());
  auto aHasStream = HasMediaStream(mSessionAns->GetLocalTracks());
  ASSERT_EQ(oHasStream, !GetLocalUniqueStreamIds(*mSessionOff).empty());
  ASSERT_EQ(aHasStream, !GetLocalUniqueStreamIds(*mSessionAns).empty());
  }
}

TEST_P(JsepSessionTest, RenegotiationOffererRemovesTrack)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  if (types.front() == SdpMediaSection::kApplication) {
    return;
  }

  OfferAnswer();

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  RefPtr<JsepTrack> removedTrack = GetTrackOff(0, types.front());
  ASSERT_TRUE(removedTrack);
  ASSERT_EQ(NS_OK, mSessionOff->RemoveTrack(removedTrack->GetStreamId(),
                                           removedTrack->GetTrackId()));

  OfferAnswer(CHECK_SUCCESS);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(1U, removed.size());

  ASSERT_EQ(removedTrack->GetMediaType(), removed[0]->GetMediaType());
  ASSERT_EQ(removedTrack->GetStreamId(), removed[0]->GetStreamId());
  ASSERT_EQ(removedTrack->GetTrackId(), removed[0]->GetTrackId());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  // First m-section should be recvonly
  auto offer = GetParsedLocalDescription(*mSessionOff);
  auto* msection = GetMsection(*offer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_TRUE(msection->IsReceiving());
  ASSERT_FALSE(msection->IsSending());

  // First audio m-section should be sendonly
  auto answer = GetParsedLocalDescription(*mSessionAns);
  msection = GetMsection(*answer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_FALSE(msection->IsReceiving());
  ASSERT_TRUE(msection->IsSending());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  // Will be the same size since we still have a track on one side.
  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());

  // This should be the only difference.
  ASSERT_TRUE(offererPairs[0].mSending);
  ASSERT_FALSE(newOffererPairs[0].mSending);

  // Remove this difference, let loop below take care of the rest
  offererPairs[0].mSending = nullptr;
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }

  // Will be the same size since we still have a track on one side.
  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size());

  // This should be the only difference.
  ASSERT_TRUE(answererPairs[0].mReceiving);
  ASSERT_FALSE(newAnswererPairs[0].mReceiving);

  // Remove this difference, let loop below take care of the rest
  answererPairs[0].mReceiving = nullptr;
  for (size_t i = 0; i < answererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newAnswererPairs[i]));
  }
}

TEST_P(JsepSessionTest, RenegotiationAnswererRemovesTrack)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  if (types.front() == SdpMediaSection::kApplication) {
    return;
  }

  OfferAnswer();

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  RefPtr<JsepTrack> removedTrack = GetTrackAns(0, types.front());
  ASSERT_TRUE(removedTrack);
  ASSERT_EQ(NS_OK, mSessionAns->RemoveTrack(removedTrack->GetStreamId(),
                                           removedTrack->GetTrackId()));

  OfferAnswer(CHECK_SUCCESS);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(1U, removed.size());

  ASSERT_EQ(removedTrack->GetMediaType(), removed[0]->GetMediaType());
  ASSERT_EQ(removedTrack->GetStreamId(), removed[0]->GetStreamId());
  ASSERT_EQ(removedTrack->GetTrackId(), removed[0]->GetTrackId());

  // First m-section should be sendrecv
  auto offer = GetParsedLocalDescription(*mSessionOff);
  auto* msection = GetMsection(*offer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_TRUE(msection->IsReceiving());
  ASSERT_TRUE(msection->IsSending());

  // First audio m-section should be recvonly
  auto answer = GetParsedLocalDescription(*mSessionAns);
  msection = GetMsection(*answer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_TRUE(msection->IsReceiving());
  ASSERT_FALSE(msection->IsSending());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  // Will be the same size since we still have a track on one side.
  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());

  // This should be the only difference.
  ASSERT_TRUE(offererPairs[0].mReceiving);
  ASSERT_FALSE(newOffererPairs[0].mReceiving);

  // Remove this difference, let loop below take care of the rest
  offererPairs[0].mReceiving = nullptr;
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }

  // Will be the same size since we still have a track on one side.
  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size());

  // This should be the only difference.
  ASSERT_TRUE(answererPairs[0].mSending);
  ASSERT_FALSE(newAnswererPairs[0].mSending);

  // Remove this difference, let loop below take care of the rest
  answererPairs[0].mSending = nullptr;
  for (size_t i = 0; i < answererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newAnswererPairs[i]));
  }
}

TEST_P(JsepSessionTest, RenegotiationBothRemoveTrack)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  if (types.front() == SdpMediaSection::kApplication) {
    return;
  }

  OfferAnswer();

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  RefPtr<JsepTrack> removedTrackAnswer = GetTrackAns(0, types.front());
  ASSERT_TRUE(removedTrackAnswer);
  ASSERT_EQ(NS_OK, mSessionAns->RemoveTrack(removedTrackAnswer->GetStreamId(),
                                           removedTrackAnswer->GetTrackId()));

  RefPtr<JsepTrack> removedTrackOffer = GetTrackOff(0, types.front());
  ASSERT_TRUE(removedTrackOffer);
  ASSERT_EQ(NS_OK, mSessionOff->RemoveTrack(removedTrackOffer->GetStreamId(),
                                           removedTrackOffer->GetTrackId()));

  OfferAnswer(CHECK_SUCCESS);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(1U, removed.size());

  ASSERT_EQ(removedTrackOffer->GetMediaType(), removed[0]->GetMediaType());
  ASSERT_EQ(removedTrackOffer->GetStreamId(), removed[0]->GetStreamId());
  ASSERT_EQ(removedTrackOffer->GetTrackId(), removed[0]->GetTrackId());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(1U, removed.size());

  ASSERT_EQ(removedTrackAnswer->GetMediaType(), removed[0]->GetMediaType());
  ASSERT_EQ(removedTrackAnswer->GetStreamId(), removed[0]->GetStreamId());
  ASSERT_EQ(removedTrackAnswer->GetTrackId(), removed[0]->GetTrackId());

  // First m-section should be recvonly
  auto offer = GetParsedLocalDescription(*mSessionOff);
  auto* msection = GetMsection(*offer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_TRUE(msection->IsReceiving());
  ASSERT_FALSE(msection->IsSending());

  // First m-section should be inactive, and rejected
  auto answer = GetParsedLocalDescription(*mSessionAns);
  msection = GetMsection(*answer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_FALSE(msection->IsReceiving());
  ASSERT_FALSE(msection->IsSending());
  ASSERT_FALSE(msection->GetPort());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size(), newOffererPairs.size() + 1);

  for (size_t i = 0; i < newOffererPairs.size(); ++i) {
    JsepTrackPair oldPair(offererPairs[i + 1]);
    JsepTrackPair newPair(newOffererPairs[i]);
    ASSERT_EQ(oldPair.mLevel, newPair.mLevel);
    ASSERT_EQ(oldPair.mSending.get(), newPair.mSending.get());
    ASSERT_EQ(oldPair.mReceiving.get(), newPair.mReceiving.get());
    ASSERT_TRUE(oldPair.HasBundleLevel());
    ASSERT_TRUE(newPair.HasBundleLevel());
    ASSERT_EQ(0U, oldPair.BundleLevel());
    ASSERT_EQ(1U, newPair.BundleLevel());
  }

  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size() + 1);

  for (size_t i = 0; i < newAnswererPairs.size(); ++i) {
    JsepTrackPair oldPair(answererPairs[i + 1]);
    JsepTrackPair newPair(newAnswererPairs[i]);
    ASSERT_EQ(oldPair.mLevel, newPair.mLevel);
    ASSERT_EQ(oldPair.mSending.get(), newPair.mSending.get());
    ASSERT_EQ(oldPair.mReceiving.get(), newPair.mReceiving.get());
    ASSERT_TRUE(oldPair.HasBundleLevel());
    ASSERT_TRUE(newPair.BundleLevel());
    ASSERT_EQ(0U, oldPair.BundleLevel());
    ASSERT_EQ(1U, newPair.BundleLevel());
  }
}

TEST_P(JsepSessionTest, RenegotiationBothRemoveThenAddTrack)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  if (types.front() == SdpMediaSection::kApplication) {
    return;
  }

  SdpMediaSection::MediaType removedType = types.front();

  OfferAnswer();

  RefPtr<JsepTrack> removedTrackAnswer = GetTrackAns(0, removedType);
  ASSERT_TRUE(removedTrackAnswer);
  ASSERT_EQ(NS_OK, mSessionAns->RemoveTrack(removedTrackAnswer->GetStreamId(),
                                           removedTrackAnswer->GetTrackId()));

  RefPtr<JsepTrack> removedTrackOffer = GetTrackOff(0, removedType);
  ASSERT_TRUE(removedTrackOffer);
  ASSERT_EQ(NS_OK, mSessionOff->RemoveTrack(removedTrackOffer->GetStreamId(),
                                           removedTrackOffer->GetTrackId()));

  OfferAnswer(CHECK_SUCCESS);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  std::vector<SdpMediaSection::MediaType> extraTypes;
  extraTypes.push_back(removedType);
  AddTracks(*mSessionAns, extraTypes);
  AddTracks(*mSessionOff, extraTypes);
  types.insert(types.end(), extraTypes.begin(), extraTypes.end());

  OfferAnswer(CHECK_SUCCESS);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(1U, added.size());
  ASSERT_EQ(0U, removed.size());
  ASSERT_EQ(removedType, added[0]->GetMediaType());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(1U, added.size());
  ASSERT_EQ(0U, removed.size());
  ASSERT_EQ(removedType, added[0]->GetMediaType());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size() + 1, newOffererPairs.size());
  ASSERT_EQ(answererPairs.size() + 1, newAnswererPairs.size());

  // Ensure that the m-section was re-used; no gaps
  for (size_t i = 0; i < newOffererPairs.size(); ++i) {
    ASSERT_EQ(i, newOffererPairs[i].mLevel);
  }
  for (size_t i = 0; i < newAnswererPairs.size(); ++i) {
    ASSERT_EQ(i, newAnswererPairs[i].mLevel);
  }
}

TEST_P(JsepSessionTest, RenegotiationBothRemoveTrackDifferentMsection)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  if (types.front() == SdpMediaSection::kApplication) {
    return;
  }

  if (types.size() < 2 || types[0] != types[1]) {
    // For simplicity, just run in cases where we have two of the same type
    return;
  }

  OfferAnswer();

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  RefPtr<JsepTrack> removedTrackAnswer = GetTrackAns(0, types.front());
  ASSERT_TRUE(removedTrackAnswer);
  ASSERT_EQ(NS_OK, mSessionAns->RemoveTrack(removedTrackAnswer->GetStreamId(),
                                           removedTrackAnswer->GetTrackId()));

  // Second instance of the same type
  RefPtr<JsepTrack> removedTrackOffer = GetTrackOff(1, types.front());
  ASSERT_TRUE(removedTrackOffer);
  ASSERT_EQ(NS_OK, mSessionOff->RemoveTrack(removedTrackOffer->GetStreamId(),
                                           removedTrackOffer->GetTrackId()));

  OfferAnswer(CHECK_SUCCESS);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(1U, removed.size());

  ASSERT_EQ(removedTrackOffer->GetMediaType(), removed[0]->GetMediaType());
  ASSERT_EQ(removedTrackOffer->GetStreamId(), removed[0]->GetStreamId());
  ASSERT_EQ(removedTrackOffer->GetTrackId(), removed[0]->GetTrackId());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(1U, removed.size());

  ASSERT_EQ(removedTrackAnswer->GetMediaType(), removed[0]->GetMediaType());
  ASSERT_EQ(removedTrackAnswer->GetStreamId(), removed[0]->GetStreamId());
  ASSERT_EQ(removedTrackAnswer->GetTrackId(), removed[0]->GetTrackId());

  // Second m-section should be recvonly
  auto offer = GetParsedLocalDescription(*mSessionOff);
  auto* msection = GetMsection(*offer, types.front(), 1);
  ASSERT_TRUE(msection);
  ASSERT_TRUE(msection->IsReceiving());
  ASSERT_FALSE(msection->IsSending());

  // First m-section should be recvonly
  auto answer = GetParsedLocalDescription(*mSessionAns);
  msection = GetMsection(*answer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_TRUE(msection->IsReceiving());
  ASSERT_FALSE(msection->IsSending());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());

  // This should be the only difference.
  ASSERT_TRUE(offererPairs[0].mReceiving);
  ASSERT_FALSE(newOffererPairs[0].mReceiving);

  // Remove this difference, let loop below take care of the rest
  offererPairs[0].mReceiving = nullptr;

  // This should be the only difference.
  ASSERT_TRUE(offererPairs[1].mSending);
  ASSERT_FALSE(newOffererPairs[1].mSending);

  // Remove this difference, let loop below take care of the rest
  offererPairs[1].mSending = nullptr;

  for (size_t i = 0; i < newOffererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }

  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size());

  // This should be the only difference.
  ASSERT_TRUE(answererPairs[0].mSending);
  ASSERT_FALSE(newAnswererPairs[0].mSending);

  // Remove this difference, let loop below take care of the rest
  answererPairs[0].mSending = nullptr;

  // This should be the only difference.
  ASSERT_TRUE(answererPairs[1].mReceiving);
  ASSERT_FALSE(newAnswererPairs[1].mReceiving);

  // Remove this difference, let loop below take care of the rest
  answererPairs[1].mReceiving = nullptr;

  for (size_t i = 0; i < newAnswererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newAnswererPairs[i]));
  }
}

TEST_P(JsepSessionTest, RenegotiationOffererReplacesTrack)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  if (types.front() == SdpMediaSection::kApplication) {
    return;
  }

  OfferAnswer();

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  RefPtr<JsepTrack> removedTrack = GetTrackOff(0, types.front());
  ASSERT_TRUE(removedTrack);
  ASSERT_EQ(NS_OK, mSessionOff->RemoveTrack(removedTrack->GetStreamId(),
                                           removedTrack->GetTrackId()));
  RefPtr<JsepTrack> addedTrack(
      new JsepTrack(types.front(), "newstream", "newtrack"));
  ASSERT_EQ(NS_OK, mSessionOff->AddTrack(addedTrack));

  OfferAnswer(CHECK_SUCCESS);

  auto added = mSessionAns->GetRemoteTracksAdded();
  auto removed = mSessionAns->GetRemoteTracksRemoved();
  ASSERT_EQ(1U, added.size());
  ASSERT_EQ(1U, removed.size());

  ASSERT_EQ(removedTrack->GetMediaType(), removed[0]->GetMediaType());
  ASSERT_EQ(removedTrack->GetStreamId(), removed[0]->GetStreamId());
  ASSERT_EQ(removedTrack->GetTrackId(), removed[0]->GetTrackId());

  ASSERT_EQ(addedTrack->GetMediaType(), added[0]->GetMediaType());
  ASSERT_EQ(addedTrack->GetStreamId(), added[0]->GetStreamId());
  ASSERT_EQ(addedTrack->GetTrackId(), added[0]->GetTrackId());

  added = mSessionOff->GetRemoteTracksAdded();
  removed = mSessionOff->GetRemoteTracksRemoved();
  ASSERT_EQ(0U, added.size());
  ASSERT_EQ(0U, removed.size());

  // First audio m-section should be sendrecv
  auto offer = GetParsedLocalDescription(*mSessionOff);
  auto* msection = GetMsection(*offer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_TRUE(msection->IsReceiving());
  ASSERT_TRUE(msection->IsSending());

  // First audio m-section should be sendrecv
  auto answer = GetParsedLocalDescription(*mSessionAns);
  msection = GetMsection(*answer, types.front(), 0);
  ASSERT_TRUE(msection);
  ASSERT_TRUE(msection->IsReceiving());
  ASSERT_TRUE(msection->IsSending());

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());

  ASSERT_NE(offererPairs[0].mSending->GetStreamId(),
            newOffererPairs[0].mSending->GetStreamId());
  ASSERT_NE(offererPairs[0].mSending->GetTrackId(),
            newOffererPairs[0].mSending->GetTrackId());

  // Skip first pair
  for (size_t i = 1; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }

  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size());

  ASSERT_NE(answererPairs[0].mReceiving->GetStreamId(),
            newAnswererPairs[0].mReceiving->GetStreamId());
  ASSERT_NE(answererPairs[0].mReceiving->GetTrackId(),
            newAnswererPairs[0].mReceiving->GetTrackId());

  // Skip first pair
  for (size_t i = 1; i < newAnswererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(answererPairs[i], newAnswererPairs[i]));
  }
}

// Tests whether auto-assigned remote msids (ie; what happens when the other
// side doesn't use msid attributes) are stable across renegotiation.
TEST_P(JsepSessionTest, RenegotiationAutoAssignedMsidIsStable)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);

  DisableMsid(&answer);

  SetRemoteAnswer(answer, CHECK_SUCCESS);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);

  // Make sure that DisableMsid actually worked, since it is kinda hacky
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);
  ASSERT_EQ(offererPairs.size(), answererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(offererPairs[i].mReceiving);
    ASSERT_TRUE(answererPairs[i].mSending);
    // These should not match since we've monkeyed with the msid
    ASSERT_NE(offererPairs[i].mReceiving->GetStreamId(),
              answererPairs[i].mSending->GetStreamId());
    ASSERT_NE(offererPairs[i].mReceiving->GetTrackId(),
              answererPairs[i].mSending->GetTrackId());
  }

  offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  answer = CreateAnswer();
  SetLocalAnswer(answer);

  DisableMsid(&answer);

  SetRemoteAnswer(answer, CHECK_SUCCESS);

  auto newOffererPairs = mSessionOff->GetNegotiatedTrackPairs();

  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_TRUE(Equals(offererPairs[i], newOffererPairs[i]));
  }
}

TEST_P(JsepSessionTest, RenegotiationOffererDisablesTelephoneEvent)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  OfferAnswer();

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);

  // check all the audio tracks to make sure they have 2 codecs (109 and 101),
  // and dtmf is enabled on all audio tracks
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    std::vector<JsepTrack*> tracks;
    tracks.push_back(offererPairs[i].mSending.get());
    tracks.push_back(offererPairs[i].mReceiving.get());
    for (JsepTrack *track : tracks) {
      if (track->GetMediaType() != SdpMediaSection::kAudio) {
        continue;
      }
      const JsepTrackNegotiatedDetails* details = track->GetNegotiatedDetails();
      ASSERT_EQ(1U, details->GetEncodingCount());
      const JsepTrackEncoding& encoding = details->GetEncoding(0);
      ASSERT_EQ(2U, encoding.GetCodecs().size());
      ASSERT_TRUE(encoding.HasFormat("109"));
      ASSERT_TRUE(encoding.HasFormat("101"));
      for (JsepCodecDescription* codec: encoding.GetCodecs()) {
        ASSERT_TRUE(codec);
        // we can cast here because we've already checked for audio track
        JsepAudioCodecDescription *audioCodec =
            static_cast<JsepAudioCodecDescription*>(codec);
        ASSERT_TRUE(audioCodec->mDtmfEnabled);
      }
    }
  }

  std::string offer = CreateOffer();
  ReplaceInSdp(&offer, " 109 101 ", " 109 ");
  ReplaceInSdp(&offer, "a=fmtp:101 0-15\r\n", "");
  ReplaceInSdp(&offer, "a=rtpmap:101 telephone-event/8000/1\r\n", "");
  std::cerr << "modified OFFER: " << offer << std::endl;

  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);

  // check all the audio tracks to make sure they have 1 codec (109),
  // and dtmf is disabled on all audio tracks
  for (size_t i = 0; i < newOffererPairs.size(); ++i) {
    std::vector<JsepTrack*> tracks;
    tracks.push_back(newOffererPairs[i].mSending.get());
    tracks.push_back(newOffererPairs[i].mReceiving.get());
    for (JsepTrack* track : tracks) {
      if (track->GetMediaType() != SdpMediaSection::kAudio) {
        continue;
      }
      const JsepTrackNegotiatedDetails* details = track->GetNegotiatedDetails();
      ASSERT_EQ(1U, details->GetEncodingCount());
      const JsepTrackEncoding& encoding = details->GetEncoding(0);
      ASSERT_EQ(1U, encoding.GetCodecs().size());
      ASSERT_TRUE(encoding.HasFormat("109"));
      // we can cast here because we've already checked for audio track
      JsepAudioCodecDescription *audioCodec =
          static_cast<JsepAudioCodecDescription*>(encoding.GetCodecs()[0]);
      ASSERT_TRUE(audioCodec);
      ASSERT_FALSE(audioCodec->mDtmfEnabled);
    }
  }
}

// Tests behavior when the answerer does not use msid in the initial exchange,
// but does on renegotiation.
TEST_P(JsepSessionTest, RenegotiationAnswererEnablesMsid)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);

  DisableMsid(&answer);

  SetRemoteAnswer(answer, CHECK_SUCCESS);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);

  offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer, CHECK_SUCCESS);

  auto newOffererPairs = mSessionOff->GetNegotiatedTrackPairs();

  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_EQ(offererPairs[i].mReceiving->GetMediaType(),
              newOffererPairs[i].mReceiving->GetMediaType());

    ASSERT_EQ(offererPairs[i].mSending, newOffererPairs[i].mSending);
    ASSERT_TRUE(Equals(offererPairs[i].mRtpTransport,
                       newOffererPairs[i].mRtpTransport));
    ASSERT_TRUE(Equals(offererPairs[i].mRtcpTransport,
                       newOffererPairs[i].mRtcpTransport));

    if (offererPairs[i].mReceiving->GetMediaType() ==
        SdpMediaSection::kApplication) {
      ASSERT_EQ(offererPairs[i].mReceiving, newOffererPairs[i].mReceiving);
    } else {
      // This should be the only difference
      ASSERT_NE(offererPairs[i].mReceiving, newOffererPairs[i].mReceiving);
    }
  }
}

TEST_P(JsepSessionTest, RenegotiationAnswererDisablesMsid)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer, CHECK_SUCCESS);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);

  offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  AddTracks(*mSessionAns);
  answer = CreateAnswer();
  SetLocalAnswer(answer);

  DisableMsid(&answer);

  SetRemoteAnswer(answer, CHECK_SUCCESS);

  auto newOffererPairs = mSessionOff->GetNegotiatedTrackPairs();

  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());
  for (size_t i = 0; i < offererPairs.size(); ++i) {
    ASSERT_EQ(offererPairs[i].mReceiving->GetMediaType(),
              newOffererPairs[i].mReceiving->GetMediaType());

    ASSERT_EQ(offererPairs[i].mSending, newOffererPairs[i].mSending);
    ASSERT_TRUE(Equals(offererPairs[i].mRtpTransport,
                       newOffererPairs[i].mRtpTransport));
    ASSERT_TRUE(Equals(offererPairs[i].mRtcpTransport,
                       newOffererPairs[i].mRtcpTransport));

    if (offererPairs[i].mReceiving->GetMediaType() ==
        SdpMediaSection::kApplication) {
      ASSERT_EQ(offererPairs[i].mReceiving, newOffererPairs[i].mReceiving);
    } else {
      // This should be the only difference
      ASSERT_NE(offererPairs[i].mReceiving, newOffererPairs[i].mReceiving);
    }
  }
}

// Tests behavior when offerer does not use bundle on the initial offer/answer,
// but does on renegotiation.
TEST_P(JsepSessionTest, RenegotiationOffererEnablesBundle)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  if (types.size() < 2) {
    // No bundle will happen here.
    return;
  }

  std::string offer = CreateOffer();

  DisableBundle(&offer);

  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  OfferAnswer();

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(newOffererPairs.size(), newAnswererPairs.size());
  ASSERT_EQ(offererPairs.size(), newOffererPairs.size());
  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size());

  for (size_t i = 0; i < newOffererPairs.size(); ++i) {
    // No bundle initially
    ASSERT_FALSE(offererPairs[i].HasBundleLevel());
    ASSERT_FALSE(answererPairs[i].HasBundleLevel());
    if (i != 0) {
      ASSERT_NE(offererPairs[0].mRtpTransport.get(),
                offererPairs[i].mRtpTransport.get());
      if (offererPairs[0].mRtcpTransport) {
        ASSERT_NE(offererPairs[0].mRtcpTransport.get(),
                  offererPairs[i].mRtcpTransport.get());
      }
      ASSERT_NE(answererPairs[0].mRtpTransport.get(),
                answererPairs[i].mRtpTransport.get());
      if (answererPairs[0].mRtcpTransport) {
        ASSERT_NE(answererPairs[0].mRtcpTransport.get(),
                  answererPairs[i].mRtcpTransport.get());
      }
    }

    // Verify that bundle worked after renegotiation
    ASSERT_TRUE(newOffererPairs[i].HasBundleLevel());
    ASSERT_TRUE(newAnswererPairs[i].HasBundleLevel());
    ASSERT_EQ(newOffererPairs[0].mRtpTransport.get(),
              newOffererPairs[i].mRtpTransport.get());
    ASSERT_EQ(newOffererPairs[0].mRtcpTransport.get(),
              newOffererPairs[i].mRtcpTransport.get());
    ASSERT_EQ(newAnswererPairs[0].mRtpTransport.get(),
              newAnswererPairs[i].mRtpTransport.get());
    ASSERT_EQ(newAnswererPairs[0].mRtcpTransport.get(),
              newAnswererPairs[i].mRtcpTransport.get());
  }
}

TEST_P(JsepSessionTest, RenegotiationOffererDisablesBundleTransport)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  if (types.size() < 2) {
    return;
  }

  OfferAnswer();

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  std::string reoffer = CreateOffer();

  DisableMsection(&reoffer, 0);

  SetLocalOffer(reoffer, CHECK_SUCCESS);
  SetRemoteOffer(reoffer, CHECK_SUCCESS);
  std::string reanswer = CreateAnswer();
  SetLocalAnswer(reanswer, CHECK_SUCCESS);
  SetRemoteAnswer(reanswer, CHECK_SUCCESS);

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(newOffererPairs.size(), newAnswererPairs.size());
  ASSERT_EQ(offererPairs.size(), newOffererPairs.size() + 1);
  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size() + 1);

  for (size_t i = 0; i < newOffererPairs.size(); ++i) {
    ASSERT_TRUE(newOffererPairs[i].HasBundleLevel());
    ASSERT_TRUE(newAnswererPairs[i].HasBundleLevel());
    ASSERT_EQ(1U, newOffererPairs[i].BundleLevel());
    ASSERT_EQ(1U, newAnswererPairs[i].BundleLevel());
    ASSERT_EQ(newOffererPairs[0].mRtpTransport.get(),
              newOffererPairs[i].mRtpTransport.get());
    ASSERT_EQ(newOffererPairs[0].mRtcpTransport.get(),
              newOffererPairs[i].mRtcpTransport.get());
    ASSERT_EQ(newAnswererPairs[0].mRtpTransport.get(),
              newAnswererPairs[i].mRtpTransport.get());
    ASSERT_EQ(newAnswererPairs[0].mRtcpTransport.get(),
              newAnswererPairs[i].mRtcpTransport.get());
  }

  ASSERT_NE(newOffererPairs[0].mRtpTransport.get(),
            offererPairs[0].mRtpTransport.get());
  ASSERT_NE(newAnswererPairs[0].mRtpTransport.get(),
            answererPairs[0].mRtpTransport.get());

  ASSERT_LE(1U, mSessionOff->GetTransports().size());
  ASSERT_LE(1U, mSessionAns->GetTransports().size());

  ASSERT_EQ(0U, mSessionOff->GetTransports()[0]->mComponents);
  ASSERT_EQ(0U, mSessionAns->GetTransports()[0]->mComponents);
}

TEST_P(JsepSessionTest, RenegotiationAnswererDisablesBundleTransport)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  if (types.size() < 2) {
    return;
  }

  OfferAnswer();

  auto offererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto answererPairs = GetTrackPairsByLevel(*mSessionAns);

  std::string reoffer = CreateOffer();
  SetLocalOffer(reoffer, CHECK_SUCCESS);
  SetRemoteOffer(reoffer, CHECK_SUCCESS);
  std::string reanswer = CreateAnswer();

  CopyTransportAttributes(&reanswer, 0, 1);
  DisableMsection(&reanswer, 0);

  SetLocalAnswer(reanswer, CHECK_SUCCESS);
  SetRemoteAnswer(reanswer, CHECK_SUCCESS);

  auto newOffererPairs = GetTrackPairsByLevel(*mSessionOff);
  auto newAnswererPairs = GetTrackPairsByLevel(*mSessionAns);

  ASSERT_EQ(newOffererPairs.size(), newAnswererPairs.size());
  ASSERT_EQ(offererPairs.size(), newOffererPairs.size() + 1);
  ASSERT_EQ(answererPairs.size(), newAnswererPairs.size() + 1);

  for (size_t i = 0; i < newOffererPairs.size(); ++i) {
    ASSERT_TRUE(newOffererPairs[i].HasBundleLevel());
    ASSERT_TRUE(newAnswererPairs[i].HasBundleLevel());
    ASSERT_EQ(1U, newOffererPairs[i].BundleLevel());
    ASSERT_EQ(1U, newAnswererPairs[i].BundleLevel());
    ASSERT_EQ(newOffererPairs[0].mRtpTransport.get(),
              newOffererPairs[i].mRtpTransport.get());
    ASSERT_EQ(newOffererPairs[0].mRtcpTransport.get(),
              newOffererPairs[i].mRtcpTransport.get());
    ASSERT_EQ(newAnswererPairs[0].mRtpTransport.get(),
              newAnswererPairs[i].mRtpTransport.get());
    ASSERT_EQ(newAnswererPairs[0].mRtcpTransport.get(),
              newAnswererPairs[i].mRtcpTransport.get());
  }

  ASSERT_NE(newOffererPairs[0].mRtpTransport.get(),
            offererPairs[0].mRtpTransport.get());
  ASSERT_NE(newAnswererPairs[0].mRtpTransport.get(),
            answererPairs[0].mRtpTransport.get());
}

TEST_P(JsepSessionTest, ParseRejectsBadMediaFormat)
{
  if (GetParam() == "datachannel") {
    return;
  }
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  UniquePtr<Sdp> munge(Parse(offer));
  SdpMediaSection& mediaSection = munge->GetMediaSection(0);
  mediaSection.AddCodec("75", "DummyFormatVal", 8000, 1);
  std::string sdpString = munge->ToString();
  nsresult rv = mSessionOff->SetLocalDescription(kJsepSdpOffer, sdpString);
  ASSERT_EQ(NS_ERROR_INVALID_ARG, rv);
}

TEST_P(JsepSessionTest, FullCallWithCandidates)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  mOffCandidates->Gather(*mSessionOff, types);

  UniquePtr<Sdp> localOffer(Parse(
        mSessionOff->GetLocalDescription(kJsepDescriptionPending)));
  for (size_t i = 0; i < localOffer->GetMediaSectionCount(); ++i) {
    mOffCandidates->CheckRtpCandidates(
        true, localOffer->GetMediaSection(i), i,
        "Local offer after gathering should have RTP candidates.");
    mOffCandidates->CheckDefaultRtpCandidate(
        true, localOffer->GetMediaSection(i), i,
        "Local offer after gathering should have a default RTP candidate.");
    mOffCandidates->CheckRtcpCandidates(
        types[i] != SdpMediaSection::kApplication,
        localOffer->GetMediaSection(i), i,
        "Local offer after gathering should have RTCP candidates "
        "(unless m=application)");
    mOffCandidates->CheckDefaultRtcpCandidate(
        types[i] != SdpMediaSection::kApplication,
        localOffer->GetMediaSection(i), i,
        "Local offer after gathering should have a default RTCP candidate "
        "(unless m=application)");
    CheckEndOfCandidates(true, localOffer->GetMediaSection(i),
        "Local offer after gathering should have an end-of-candidates.");
  }

  SetRemoteOffer(offer);
  mOffCandidates->Trickle(*mSessionAns);

  UniquePtr<Sdp> remoteOffer(Parse(
        mSessionAns->GetRemoteDescription(kJsepDescriptionPending)));
  for (size_t i = 0; i < remoteOffer->GetMediaSectionCount(); ++i) {
    mOffCandidates->CheckRtpCandidates(
        true, remoteOffer->GetMediaSection(i), i,
        "Remote offer after trickle should have RTP candidates.");
    mOffCandidates->CheckDefaultRtpCandidate(
        false, remoteOffer->GetMediaSection(i), i,
        "Initial remote offer should not have a default RTP candidate.");
    mOffCandidates->CheckRtcpCandidates(
        types[i] != SdpMediaSection::kApplication,
        remoteOffer->GetMediaSection(i), i,
        "Remote offer after trickle should have RTCP candidates "
        "(unless m=application)");
    mOffCandidates->CheckDefaultRtcpCandidate(
        false, remoteOffer->GetMediaSection(i), i,
        "Initial remote offer should not have a default RTCP candidate.");
    CheckEndOfCandidates(false, remoteOffer->GetMediaSection(i),
        "Initial remote offer should not have an end-of-candidates.");
  }

  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  // This will gather candidates that mSessionAns knows it doesn't need.
  // They should not be present in the SDP.
  mAnsCandidates->Gather(*mSessionAns, types);

  UniquePtr<Sdp> localAnswer(Parse(
        mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)));
  for (size_t i = 0; i < localAnswer->GetMediaSectionCount(); ++i) {
    mAnsCandidates->CheckRtpCandidates(
        i == 0, localAnswer->GetMediaSection(i), i,
        "Local answer after gathering should have RTP candidates on level 0.");
    mAnsCandidates->CheckDefaultRtpCandidate(
        true, localAnswer->GetMediaSection(i), 0,
        "Local answer after gathering should have a default RTP candidate "
        "on all levels that matches transport level 0.");
    mAnsCandidates->CheckRtcpCandidates(
        false, localAnswer->GetMediaSection(i), i,
        "Local answer after gathering should not have RTCP candidates "
        "(because we're answering with rtcp-mux)");
    mAnsCandidates->CheckDefaultRtcpCandidate(
        false, localAnswer->GetMediaSection(i), i,
        "Local answer after gathering should not have a default RTCP candidate "
        "(because we're answering with rtcp-mux)");
    CheckEndOfCandidates(i == 0, localAnswer->GetMediaSection(i),
        "Local answer after gathering should have an end-of-candidates only for"
        " level 0.");
  }

  SetRemoteAnswer(answer);
  mAnsCandidates->Trickle(*mSessionOff);

  UniquePtr<Sdp> remoteAnswer(Parse(
        mSessionOff->GetRemoteDescription(kJsepDescriptionCurrent)));
  for (size_t i = 0; i < remoteAnswer->GetMediaSectionCount(); ++i) {
    mAnsCandidates->CheckRtpCandidates(
        i == 0, remoteAnswer->GetMediaSection(i), i,
        "Remote answer after trickle should have RTP candidates on level 0.");
    mAnsCandidates->CheckDefaultRtpCandidate(
        false, remoteAnswer->GetMediaSection(i), i,
        "Remote answer after trickle should not have a default RTP candidate.");
    mAnsCandidates->CheckRtcpCandidates(
        false, remoteAnswer->GetMediaSection(i), i,
        "Remote answer after trickle should not have RTCP candidates "
        "(because we're answering with rtcp-mux)");
    mAnsCandidates->CheckDefaultRtcpCandidate(
        false, remoteAnswer->GetMediaSection(i), i,
        "Remote answer after trickle should not have a default RTCP "
        "candidate.");
    CheckEndOfCandidates(false, remoteAnswer->GetMediaSection(i),
        "Remote answer after trickle should not have an end-of-candidates.");
  }
}

TEST_P(JsepSessionTest, RenegotiationWithCandidates)
{
  AddTracks(*mSessionOff);
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  mOffCandidates->Gather(*mSessionOff, types);
  SetRemoteOffer(offer);
  mOffCandidates->Trickle(*mSessionAns);
  AddTracks(*mSessionAns);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  mAnsCandidates->Gather(*mSessionAns, types);
  SetRemoteAnswer(answer);
  mAnsCandidates->Trickle(*mSessionOff);

  offer = CreateOffer();
  SetLocalOffer(offer);

  UniquePtr<Sdp> parsedOffer(Parse(offer));
  for (size_t i = 0; i < parsedOffer->GetMediaSectionCount(); ++i) {
    mOffCandidates->CheckRtpCandidates(
        i == 0, parsedOffer->GetMediaSection(i), i,
        "Local reoffer before gathering should have RTP candidates on level 0"
        " only.");
    mOffCandidates->CheckDefaultRtpCandidate(
        i == 0, parsedOffer->GetMediaSection(i), 0,
        "Local reoffer before gathering should have a default RTP candidate "
        "on level 0 only.");
    mOffCandidates->CheckRtcpCandidates(
        false, parsedOffer->GetMediaSection(i), i,
        "Local reoffer before gathering should not have RTCP candidates.");
    mOffCandidates->CheckDefaultRtcpCandidate(
        false, parsedOffer->GetMediaSection(i), i,
        "Local reoffer before gathering should not have a default RTCP "
        "candidate.");
    CheckEndOfCandidates(false, parsedOffer->GetMediaSection(i),
        "Local reoffer before gathering should not have an end-of-candidates.");
  }

  // mSessionAns should generate a reoffer that is similar
  std::string otherOffer;
  JsepOfferOptions defaultOptions;
  nsresult rv = mSessionAns->CreateOffer(defaultOptions, &otherOffer);
  ASSERT_EQ(NS_OK, rv);
  parsedOffer = Parse(otherOffer);
  for (size_t i = 0; i < parsedOffer->GetMediaSectionCount(); ++i) {
    mAnsCandidates->CheckRtpCandidates(
        i == 0, parsedOffer->GetMediaSection(i), i,
        "Local reoffer before gathering should have RTP candidates on level 0"
        " only. (previous answerer)");
    mAnsCandidates->CheckDefaultRtpCandidate(
        i == 0, parsedOffer->GetMediaSection(i), 0,
        "Local reoffer before gathering should have a default RTP candidate "
        "on level 0 only. (previous answerer)");
    mAnsCandidates->CheckRtcpCandidates(
        false, parsedOffer->GetMediaSection(i), i,
        "Local reoffer before gathering should not have RTCP candidates."
        " (previous answerer)");
    mAnsCandidates->CheckDefaultRtcpCandidate(
        false, parsedOffer->GetMediaSection(i), i,
        "Local reoffer before gathering should not have a default RTCP "
        "candidate. (previous answerer)");
    CheckEndOfCandidates(false, parsedOffer->GetMediaSection(i),
        "Local reoffer before gathering should not have an end-of-candidates. "
        "(previous answerer)");
  }

  // Ok, let's continue with the renegotiation
  SetRemoteOffer(offer);

  // PeerConnection will not re-gather for RTP, but it will for RTCP in case
  // the answerer decides to turn off rtcp-mux.
  if (types[0] != SdpMediaSection::kApplication) {
    mOffCandidates->Gather(*mSessionOff, 0, RTCP);
  }

  // Since the remaining levels were bundled, PeerConnection will re-gather for
  // both RTP and RTCP, in case the answerer rejects bundle.
  for (size_t level = 1; level < types.size(); ++level) {
    mOffCandidates->Gather(*mSessionOff, level, RTP);
    if (types[level] != SdpMediaSection::kApplication) {
      mOffCandidates->Gather(*mSessionOff, level, RTCP);
    }
  }
  mOffCandidates->FinishGathering(*mSessionOff);

  mOffCandidates->Trickle(*mSessionAns);

  UniquePtr<Sdp> localOffer(Parse(
        mSessionOff->GetLocalDescription(kJsepDescriptionPending)));
  for (size_t i = 0; i < localOffer->GetMediaSectionCount(); ++i) {
    mOffCandidates->CheckRtpCandidates(
        true, localOffer->GetMediaSection(i), i,
        "Local reoffer after gathering should have RTP candidates.");
    mOffCandidates->CheckDefaultRtpCandidate(
        true, localOffer->GetMediaSection(i), i,
        "Local reoffer after gathering should have a default RTP candidate.");
    mOffCandidates->CheckRtcpCandidates(
        types[i] != SdpMediaSection::kApplication,
        localOffer->GetMediaSection(i), i,
        "Local reoffer after gathering should have RTCP candidates "
        "(unless m=application)");
    mOffCandidates->CheckDefaultRtcpCandidate(
        types[i] != SdpMediaSection::kApplication,
        localOffer->GetMediaSection(i), i,
        "Local reoffer after gathering should have a default RTCP candidate "
        "(unless m=application)");
    CheckEndOfCandidates(true, localOffer->GetMediaSection(i),
        "Local reoffer after gathering should have an end-of-candidates.");
  }

  UniquePtr<Sdp> remoteOffer(Parse(
        mSessionAns->GetRemoteDescription(kJsepDescriptionPending)));
  for (size_t i = 0; i < remoteOffer->GetMediaSectionCount(); ++i) {
    mOffCandidates->CheckRtpCandidates(
        true, remoteOffer->GetMediaSection(i), i,
        "Remote reoffer after trickle should have RTP candidates.");
    mOffCandidates->CheckDefaultRtpCandidate(
        i == 0, remoteOffer->GetMediaSection(i), i,
        "Remote reoffer should have a default RTP candidate on level 0 "
        "(because it was gathered last offer/answer).");
    mOffCandidates->CheckRtcpCandidates(
        types[i] != SdpMediaSection::kApplication,
        remoteOffer->GetMediaSection(i), i,
        "Remote reoffer after trickle should have RTCP candidates.");
    mOffCandidates->CheckDefaultRtcpCandidate(
        false, remoteOffer->GetMediaSection(i), i,
        "Remote reoffer should not have a default RTCP candidate.");
    CheckEndOfCandidates(false, remoteOffer->GetMediaSection(i),
        "Remote reoffer should not have an end-of-candidates.");
  }

  answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);
  // No candidates should be gathered at the answerer, but default candidates
  // should be set.
  mAnsCandidates->FinishGathering(*mSessionAns);

  UniquePtr<Sdp> localAnswer(Parse(
        mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)));
  for (size_t i = 0; i < localAnswer->GetMediaSectionCount(); ++i) {
    mAnsCandidates->CheckRtpCandidates(
        i == 0, localAnswer->GetMediaSection(i), i,
        "Local reanswer after gathering should have RTP candidates on level "
        "0.");
    mAnsCandidates->CheckDefaultRtpCandidate(
        true, localAnswer->GetMediaSection(i), 0,
        "Local reanswer after gathering should have a default RTP candidate "
        "on all levels that matches transport level 0.");
    mAnsCandidates->CheckRtcpCandidates(
        false, localAnswer->GetMediaSection(i), i,
        "Local reanswer after gathering should not have RTCP candidates "
        "(because we're reanswering with rtcp-mux)");
    mAnsCandidates->CheckDefaultRtcpCandidate(
        false, localAnswer->GetMediaSection(i), i,
        "Local reanswer after gathering should not have a default RTCP "
        "candidate (because we're reanswering with rtcp-mux)");
    CheckEndOfCandidates(i == 0, localAnswer->GetMediaSection(i),
        "Local reanswer after gathering should have an end-of-candidates only "
        "for level 0.");
  }

  UniquePtr<Sdp> remoteAnswer(Parse(
        mSessionOff->GetRemoteDescription(kJsepDescriptionCurrent)));
  for (size_t i = 0; i < localAnswer->GetMediaSectionCount(); ++i) {
    mAnsCandidates->CheckRtpCandidates(
        i == 0, remoteAnswer->GetMediaSection(i), i,
        "Remote reanswer after trickle should have RTP candidates on level 0.");
    mAnsCandidates->CheckDefaultRtpCandidate(
        i == 0, remoteAnswer->GetMediaSection(i), i,
        "Remote reanswer should have a default RTP candidate on level 0 "
        "(because it was gathered last offer/answer).");
    mAnsCandidates->CheckRtcpCandidates(
        false, remoteAnswer->GetMediaSection(i), i,
        "Remote reanswer after trickle should not have RTCP candidates "
        "(because we're reanswering with rtcp-mux)");
    mAnsCandidates->CheckDefaultRtcpCandidate(
        false, remoteAnswer->GetMediaSection(i), i,
        "Remote reanswer after trickle should not have a default RTCP "
        "candidate.");
    CheckEndOfCandidates(false, remoteAnswer->GetMediaSection(i),
        "Remote reanswer after trickle should not have an end-of-candidates.");
  }
}

TEST_P(JsepSessionTest, RenegotiationAnswererSendonly)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  OfferAnswer();

  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);

  UniquePtr<Sdp> parsedAnswer(Parse(answer));
  for (size_t i = 0; i < parsedAnswer->GetMediaSectionCount(); ++i) {
    SdpMediaSection& msection = parsedAnswer->GetMediaSection(i);
    if (msection.GetMediaType() != SdpMediaSection::kApplication) {
      msection.SetReceiving(false);
    }
  }

  answer = parsedAnswer->ToString();

  SetRemoteAnswer(answer);

  for (const RefPtr<JsepTrack>& track : mSessionOff->GetLocalTracks()) {
    if (track->GetMediaType() != SdpMediaSection::kApplication) {
      ASSERT_FALSE(track->GetActive());
    }
  }

  ASSERT_EQ(types.size(), mSessionOff->GetNegotiatedTrackPairs().size());
}

TEST_P(JsepSessionTest, RenegotiationAnswererInactive)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  OfferAnswer();

  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);

  UniquePtr<Sdp> parsedAnswer(Parse(answer));
  for (size_t i = 0; i < parsedAnswer->GetMediaSectionCount(); ++i) {
    SdpMediaSection& msection = parsedAnswer->GetMediaSection(i);
    if (msection.GetMediaType() != SdpMediaSection::kApplication) {
      msection.SetReceiving(false);
      msection.SetSending(false);
    }
  }

  answer = parsedAnswer->ToString();

  SetRemoteAnswer(answer, CHECK_SUCCESS); // Won't have answerer tracks

  for (const RefPtr<JsepTrack>& track : mSessionOff->GetLocalTracks()) {
    if (track->GetMediaType() != SdpMediaSection::kApplication) {
      ASSERT_FALSE(track->GetActive());
    }
  }

  ASSERT_EQ(types.size(), mSessionOff->GetNegotiatedTrackPairs().size());
}


INSTANTIATE_TEST_CASE_P(
    Variants,
    JsepSessionTest,
    ::testing::Values("audio",
                      "video",
                      "datachannel",
                      "audio,video",
                      "video,audio",
                      "audio,datachannel",
                      "video,datachannel",
                      "video,audio,datachannel",
                      "audio,video,datachannel",
                      "datachannel,audio",
                      "datachannel,video",
                      "datachannel,audio,video",
                      "datachannel,video,audio",
                      "audio,datachannel,video",
                      "video,datachannel,audio",
                      "audio,audio",
                      "video,video",
                      "audio,audio,video",
                      "audio,video,video",
                      "audio,audio,video,video",
                      "audio,audio,video,video,datachannel"));

// offerToReceiveXxx variants

TEST_F(JsepSessionTest, OfferAnswerRecvOnlyLines)
{
  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(2U));
  std::string offer = CreateOffer(Some(options));

  UniquePtr<Sdp> parsedOffer(Parse(offer));
  ASSERT_TRUE(!!parsedOffer);

  ASSERT_EQ(3U, parsedOffer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kAudio,
            parsedOffer->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kRecvonly,
            parsedOffer->GetMediaSection(0).GetAttributeList().GetDirection());
  ASSERT_TRUE(parsedOffer->GetMediaSection(0).GetAttributeList().HasAttribute(
        SdpAttribute::kSsrcAttribute));

  ASSERT_EQ(SdpMediaSection::kVideo,
            parsedOffer->GetMediaSection(1).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kRecvonly,
            parsedOffer->GetMediaSection(1).GetAttributeList().GetDirection());
  ASSERT_TRUE(parsedOffer->GetMediaSection(1).GetAttributeList().HasAttribute(
        SdpAttribute::kSsrcAttribute));

  ASSERT_EQ(SdpMediaSection::kVideo,
            parsedOffer->GetMediaSection(2).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kRecvonly,
            parsedOffer->GetMediaSection(2).GetAttributeList().GetDirection());
  ASSERT_TRUE(parsedOffer->GetMediaSection(2).GetAttributeList().HasAttribute(
        SdpAttribute::kSsrcAttribute));

  ASSERT_TRUE(parsedOffer->GetMediaSection(0).GetAttributeList().HasAttribute(
      SdpAttribute::kRtcpMuxAttribute));
  ASSERT_TRUE(parsedOffer->GetMediaSection(1).GetAttributeList().HasAttribute(
      SdpAttribute::kRtcpMuxAttribute));
  ASSERT_TRUE(parsedOffer->GetMediaSection(2).GetAttributeList().HasAttribute(
      SdpAttribute::kRtcpMuxAttribute));

  SetLocalOffer(offer, CHECK_SUCCESS);

  AddTracks(*mSessionAns, "audio,video");
  SetRemoteOffer(offer, CHECK_SUCCESS);

  std::string answer = CreateAnswer();
  UniquePtr<Sdp> parsedAnswer(Parse(answer));

  ASSERT_EQ(3U, parsedAnswer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kAudio,
            parsedAnswer->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kSendonly,
            parsedAnswer->GetMediaSection(0).GetAttributeList().GetDirection());
  ASSERT_EQ(SdpMediaSection::kVideo,
            parsedAnswer->GetMediaSection(1).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kSendonly,
            parsedAnswer->GetMediaSection(1).GetAttributeList().GetDirection());
  ASSERT_EQ(SdpMediaSection::kVideo,
            parsedAnswer->GetMediaSection(2).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kInactive,
            parsedAnswer->GetMediaSection(2).GetAttributeList().GetDirection());

  SetLocalAnswer(answer, CHECK_SUCCESS);
  SetRemoteAnswer(answer, CHECK_SUCCESS);

  std::vector<JsepTrackPair> trackPairs(mSessionOff->GetNegotiatedTrackPairs());
  ASSERT_EQ(2U, trackPairs.size());
  for (auto pair : trackPairs) {
    auto ssrcs = parsedOffer->GetMediaSection(pair.mLevel).GetAttributeList()
                 .GetSsrc().mSsrcs;
    ASSERT_EQ(1U, ssrcs.size());
    ASSERT_EQ(pair.mRecvonlySsrc, ssrcs.front().ssrc);
  }
}

TEST_F(JsepSessionTest, OfferAnswerSendOnlyLines)
{
  AddTracks(*mSessionOff, "audio,video,video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(0U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));
  std::string offer = CreateOffer(Some(options));

  UniquePtr<Sdp> outputSdp(Parse(offer));
  ASSERT_TRUE(!!outputSdp);

  ASSERT_EQ(3U, outputSdp->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kAudio,
            outputSdp->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kSendonly,
            outputSdp->GetMediaSection(0).GetAttributeList().GetDirection());
  ASSERT_EQ(SdpMediaSection::kVideo,
            outputSdp->GetMediaSection(1).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kSendrecv,
            outputSdp->GetMediaSection(1).GetAttributeList().GetDirection());
  ASSERT_EQ(SdpMediaSection::kVideo,
            outputSdp->GetMediaSection(2).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kSendonly,
            outputSdp->GetMediaSection(2).GetAttributeList().GetDirection());

  ASSERT_TRUE(outputSdp->GetMediaSection(0).GetAttributeList().HasAttribute(
      SdpAttribute::kRtcpMuxAttribute));
  ASSERT_TRUE(outputSdp->GetMediaSection(1).GetAttributeList().HasAttribute(
      SdpAttribute::kRtcpMuxAttribute));
  ASSERT_TRUE(outputSdp->GetMediaSection(2).GetAttributeList().HasAttribute(
      SdpAttribute::kRtcpMuxAttribute));

  SetLocalOffer(offer, CHECK_SUCCESS);

  AddTracks(*mSessionAns, "audio,video");
  SetRemoteOffer(offer, CHECK_SUCCESS);

  std::string answer = CreateAnswer();
  outputSdp = Parse(answer);

  ASSERT_EQ(3U, outputSdp->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kAudio,
            outputSdp->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kRecvonly,
            outputSdp->GetMediaSection(0).GetAttributeList().GetDirection());
  ASSERT_EQ(SdpMediaSection::kVideo,
            outputSdp->GetMediaSection(1).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kSendrecv,
            outputSdp->GetMediaSection(1).GetAttributeList().GetDirection());
  ASSERT_EQ(SdpMediaSection::kVideo,
            outputSdp->GetMediaSection(2).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kRecvonly,
            outputSdp->GetMediaSection(2).GetAttributeList().GetDirection());
}

TEST_F(JsepSessionTest, OfferToReceiveAudioNotUsed)
{
  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some<size_t>(1);

  OfferAnswer(CHECK_SUCCESS, Some(options));

  UniquePtr<Sdp> offer(Parse(
        mSessionOff->GetLocalDescription(kJsepDescriptionCurrent)));
  ASSERT_TRUE(offer.get());
  ASSERT_EQ(1U, offer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kAudio,
            offer->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kRecvonly,
            offer->GetMediaSection(0).GetAttributeList().GetDirection());

  UniquePtr<Sdp> answer(Parse(
        mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)));
  ASSERT_TRUE(answer.get());
  ASSERT_EQ(1U, answer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kAudio,
            answer->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kInactive,
            answer->GetMediaSection(0).GetAttributeList().GetDirection());
}

TEST_F(JsepSessionTest, OfferToReceiveVideoNotUsed)
{
  JsepOfferOptions options;
  options.mOfferToReceiveVideo = Some<size_t>(1);

  OfferAnswer(CHECK_SUCCESS, Some(options));

  UniquePtr<Sdp> offer(Parse(
        mSessionOff->GetLocalDescription(kJsepDescriptionCurrent)));
  ASSERT_TRUE(offer.get());
  ASSERT_EQ(1U, offer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kVideo,
            offer->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kRecvonly,
            offer->GetMediaSection(0).GetAttributeList().GetDirection());

  UniquePtr<Sdp> answer(Parse(
        mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)));
  ASSERT_TRUE(answer.get());
  ASSERT_EQ(1U, answer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kVideo,
            answer->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpDirectionAttribute::kInactive,
            answer->GetMediaSection(0).GetAttributeList().GetDirection());
}

TEST_F(JsepSessionTest, CreateOfferNoDatachannelDefault)
{
  RefPtr<JsepTrack> msta(
      new JsepTrack(SdpMediaSection::kAudio, "offerer_stream", "a1"));
  mSessionOff->AddTrack(msta);

  RefPtr<JsepTrack> mstv1(
      new JsepTrack(SdpMediaSection::kVideo, "offerer_stream", "v1"));
  mSessionOff->AddTrack(mstv1);

  std::string offer = CreateOffer();

  UniquePtr<Sdp> outputSdp(Parse(offer));
  ASSERT_TRUE(!!outputSdp);

  ASSERT_EQ(2U, outputSdp->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kAudio,
            outputSdp->GetMediaSection(0).GetMediaType());
  ASSERT_EQ(SdpMediaSection::kVideo,
            outputSdp->GetMediaSection(1).GetMediaType());
}

TEST_F(JsepSessionTest, ValidateOfferedVideoCodecParams)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);

  RefPtr<JsepTrack> msta(
      new JsepTrack(SdpMediaSection::kAudio, "offerer_stream", "a1"));
  mSessionOff->AddTrack(msta);
  RefPtr<JsepTrack> mstv1(
      new JsepTrack(SdpMediaSection::kVideo, "offerer_stream", "v2"));
  mSessionOff->AddTrack(mstv1);

  std::string offer = CreateOffer();

  UniquePtr<Sdp> outputSdp(Parse(offer));
  ASSERT_TRUE(!!outputSdp);

  ASSERT_EQ(2U, outputSdp->GetMediaSectionCount());
  auto& video_section = outputSdp->GetMediaSection(1);
  ASSERT_EQ(SdpMediaSection::kVideo, video_section.GetMediaType());
  auto& video_attrs = video_section.GetAttributeList();
  ASSERT_EQ(SdpDirectionAttribute::kSendrecv, video_attrs.GetDirection());

  ASSERT_EQ(6U, video_section.GetFormats().size());
  ASSERT_EQ("120", video_section.GetFormats()[0]);
  ASSERT_EQ("121", video_section.GetFormats()[1]);
  ASSERT_EQ("126", video_section.GetFormats()[2]);
  ASSERT_EQ("97", video_section.GetFormats()[3]);
  ASSERT_EQ("122", video_section.GetFormats()[4]);
  ASSERT_EQ("123", video_section.GetFormats()[5]);

  // Validate rtpmap
  ASSERT_TRUE(video_attrs.HasAttribute(SdpAttribute::kRtpmapAttribute));
  auto& rtpmaps = video_attrs.GetRtpmap();
  ASSERT_TRUE(rtpmaps.HasEntry("120"));
  ASSERT_TRUE(rtpmaps.HasEntry("121"));
  ASSERT_TRUE(rtpmaps.HasEntry("126"));
  ASSERT_TRUE(rtpmaps.HasEntry("97"));
  ASSERT_TRUE(rtpmaps.HasEntry("122"));
  ASSERT_TRUE(rtpmaps.HasEntry("123"));

  auto& vp8_entry = rtpmaps.GetEntry("120");
  auto& vp9_entry = rtpmaps.GetEntry("121");
  auto& h264_1_entry = rtpmaps.GetEntry("126");
  auto& h264_0_entry = rtpmaps.GetEntry("97");
  auto& red_0_entry = rtpmaps.GetEntry("122");
  auto& ulpfec_0_entry = rtpmaps.GetEntry("123");

  ASSERT_EQ("VP8", vp8_entry.name);
  ASSERT_EQ("VP9", vp9_entry.name);
  ASSERT_EQ("H264", h264_1_entry.name);
  ASSERT_EQ("H264", h264_0_entry.name);
  ASSERT_EQ("red", red_0_entry.name);
  ASSERT_EQ("ulpfec", ulpfec_0_entry.name);

  // Validate fmtps
  ASSERT_TRUE(video_attrs.HasAttribute(SdpAttribute::kFmtpAttribute));
  auto& fmtps = video_attrs.GetFmtp().mFmtps;

  ASSERT_EQ(5U, fmtps.size());

  // VP8
  const SdpFmtpAttributeList::Parameters* vp8_params =
    video_section.FindFmtp("120");
  ASSERT_TRUE(vp8_params);
  ASSERT_EQ(SdpRtpmapAttributeList::kVP8, vp8_params->codec_type);

  auto& parsed_vp8_params =
      *static_cast<const SdpFmtpAttributeList::VP8Parameters*>(vp8_params);

  ASSERT_EQ((uint32_t)12288, parsed_vp8_params.max_fs);
  ASSERT_EQ((uint32_t)60, parsed_vp8_params.max_fr);

  // VP9
  const SdpFmtpAttributeList::Parameters* vp9_params =
    video_section.FindFmtp("121");
  ASSERT_TRUE(vp9_params);
  ASSERT_EQ(SdpRtpmapAttributeList::kVP9, vp9_params->codec_type);

  auto& parsed_vp9_params =
      *static_cast<const SdpFmtpAttributeList::VP8Parameters*>(vp9_params);

  ASSERT_EQ((uint32_t)12288, parsed_vp9_params.max_fs);
  ASSERT_EQ((uint32_t)60, parsed_vp9_params.max_fr);

  // H264 packetization mode 1
  const SdpFmtpAttributeList::Parameters* h264_1_params =
    video_section.FindFmtp("126");
  ASSERT_TRUE(h264_1_params);
  ASSERT_EQ(SdpRtpmapAttributeList::kH264, h264_1_params->codec_type);

  auto& parsed_h264_1_params =
      *static_cast<const SdpFmtpAttributeList::H264Parameters*>(h264_1_params);

  ASSERT_EQ((uint32_t)0x42e00d, parsed_h264_1_params.profile_level_id);
  ASSERT_TRUE(parsed_h264_1_params.level_asymmetry_allowed);
  ASSERT_EQ(1U, parsed_h264_1_params.packetization_mode);

  // H264 packetization mode 0
  const SdpFmtpAttributeList::Parameters* h264_0_params =
    video_section.FindFmtp("97");
  ASSERT_TRUE(h264_0_params);
  ASSERT_EQ(SdpRtpmapAttributeList::kH264, h264_0_params->codec_type);

  auto& parsed_h264_0_params =
      *static_cast<const SdpFmtpAttributeList::H264Parameters*>(h264_0_params);

  ASSERT_EQ((uint32_t)0x42e00d, parsed_h264_0_params.profile_level_id);
  ASSERT_TRUE(parsed_h264_0_params.level_asymmetry_allowed);
  ASSERT_EQ(0U, parsed_h264_0_params.packetization_mode);

  // red
  const SdpFmtpAttributeList::Parameters* red_params =
    video_section.FindFmtp("122");
  ASSERT_TRUE(red_params);
  ASSERT_EQ(SdpRtpmapAttributeList::kRed, red_params->codec_type);

  auto& parsed_red_params =
      *static_cast<const SdpFmtpAttributeList::RedParameters*>(red_params);
  ASSERT_EQ(5U, parsed_red_params.encodings.size());
  ASSERT_EQ(120, parsed_red_params.encodings[0]);
  ASSERT_EQ(121, parsed_red_params.encodings[1]);
  ASSERT_EQ(126, parsed_red_params.encodings[2]);
  ASSERT_EQ(97, parsed_red_params.encodings[3]);
  ASSERT_EQ(123, parsed_red_params.encodings[4]);
}

TEST_F(JsepSessionTest, ValidateOfferedAudioCodecParams)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);

  RefPtr<JsepTrack> msta(
      new JsepTrack(SdpMediaSection::kAudio, "offerer_stream", "a1"));
  mSessionOff->AddTrack(msta);
  RefPtr<JsepTrack> mstv1(
      new JsepTrack(SdpMediaSection::kVideo, "offerer_stream", "v2"));
  mSessionOff->AddTrack(mstv1);

  std::string offer = CreateOffer();

  UniquePtr<Sdp> outputSdp(Parse(offer));
  ASSERT_TRUE(!!outputSdp);

  ASSERT_EQ(2U, outputSdp->GetMediaSectionCount());
  auto& audio_section = outputSdp->GetMediaSection(0);
  ASSERT_EQ(SdpMediaSection::kAudio, audio_section.GetMediaType());
  auto& audio_attrs = audio_section.GetAttributeList();
  ASSERT_EQ(SdpDirectionAttribute::kSendrecv, audio_attrs.GetDirection());
  ASSERT_EQ(5U, audio_section.GetFormats().size());
  ASSERT_EQ("109", audio_section.GetFormats()[0]);
  ASSERT_EQ("9", audio_section.GetFormats()[1]);
  ASSERT_EQ("0", audio_section.GetFormats()[2]);
  ASSERT_EQ("8", audio_section.GetFormats()[3]);
  ASSERT_EQ("101", audio_section.GetFormats()[4]);

  // Validate rtpmap
  ASSERT_TRUE(audio_attrs.HasAttribute(SdpAttribute::kRtpmapAttribute));
  auto& rtpmaps = audio_attrs.GetRtpmap();
  ASSERT_TRUE(rtpmaps.HasEntry("109"));
  ASSERT_TRUE(rtpmaps.HasEntry("9"));
  ASSERT_TRUE(rtpmaps.HasEntry("0"));
  ASSERT_TRUE(rtpmaps.HasEntry("8"));
  ASSERT_TRUE(rtpmaps.HasEntry("101"));

  auto& opus_entry = rtpmaps.GetEntry("109");
  auto& g722_entry = rtpmaps.GetEntry("9");
  auto& pcmu_entry = rtpmaps.GetEntry("0");
  auto& pcma_entry = rtpmaps.GetEntry("8");
  auto& telephone_event_entry = rtpmaps.GetEntry("101");

  ASSERT_EQ("opus", opus_entry.name);
  ASSERT_EQ("G722", g722_entry.name);
  ASSERT_EQ("PCMU", pcmu_entry.name);
  ASSERT_EQ("PCMA", pcma_entry.name);
  ASSERT_EQ("telephone-event", telephone_event_entry.name);

  // Validate fmtps
  ASSERT_TRUE(audio_attrs.HasAttribute(SdpAttribute::kFmtpAttribute));
  auto& fmtps = audio_attrs.GetFmtp().mFmtps;

  ASSERT_EQ(2U, fmtps.size());

  // opus
  const SdpFmtpAttributeList::Parameters* opus_params =
    audio_section.FindFmtp("109");
  ASSERT_TRUE(opus_params);
  ASSERT_EQ(SdpRtpmapAttributeList::kOpus, opus_params->codec_type);

  auto& parsed_opus_params =
      *static_cast<const SdpFmtpAttributeList::OpusParameters*>(opus_params);

  ASSERT_EQ((uint32_t)48000, parsed_opus_params.maxplaybackrate);
  ASSERT_EQ((uint32_t)1, parsed_opus_params.stereo);
  ASSERT_EQ((uint32_t)0, parsed_opus_params.useInBandFec);

  // dtmf
  const SdpFmtpAttributeList::Parameters* dtmf_params =
    audio_section.FindFmtp("101");
  ASSERT_TRUE(dtmf_params);
  ASSERT_EQ(SdpRtpmapAttributeList::kTelephoneEvent, dtmf_params->codec_type);

  auto& parsed_dtmf_params =
      *static_cast<const SdpFmtpAttributeList::TelephoneEventParameters*>
          (dtmf_params);

  ASSERT_EQ("0-15", parsed_dtmf_params.dtmfTones);
}

TEST_F(JsepSessionTest, ValidateNoFmtpLineForRedInOfferAndAnswer)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);

  RefPtr<JsepTrack> msta(
      new JsepTrack(SdpMediaSection::kAudio, "offerer_stream", "a1"));
  mSessionOff->AddTrack(msta);
  RefPtr<JsepTrack> mstv1(
      new JsepTrack(SdpMediaSection::kVideo, "offerer_stream", "v1"));
  mSessionOff->AddTrack(mstv1);

  std::string offer = CreateOffer();

  // look for line with fmtp:122 and remove it
  size_t start = offer.find("a=fmtp:122");
  size_t end = offer.find("\r\n", start);
  offer.replace(start, end+2-start, "");

  SetLocalOffer(offer);
  SetRemoteOffer(offer);

  RefPtr<JsepTrack> msta_ans(
      new JsepTrack(SdpMediaSection::kAudio, "answerer_stream", "a1"));
  mSessionAns->AddTrack(msta);
  RefPtr<JsepTrack> mstv1_ans(
      new JsepTrack(SdpMediaSection::kVideo, "answerer_stream", "v1"));
  mSessionAns->AddTrack(mstv1);

  std::string answer = CreateAnswer();
  // because parsing will throw out the malformed fmtp, make sure it is not
  // in the answer sdp string
  ASSERT_EQ(std::string::npos, answer.find("a=fmtp:122"));

  UniquePtr<Sdp> outputSdp(Parse(answer));
  ASSERT_TRUE(!!outputSdp);

  ASSERT_EQ(2U, outputSdp->GetMediaSectionCount());
  auto& video_section = outputSdp->GetMediaSection(1);
  ASSERT_EQ(SdpMediaSection::kVideo, video_section.GetMediaType());
  auto& video_attrs = video_section.GetAttributeList();
  ASSERT_EQ(SdpDirectionAttribute::kSendrecv, video_attrs.GetDirection());

  ASSERT_EQ(6U, video_section.GetFormats().size());
  ASSERT_EQ("120", video_section.GetFormats()[0]);
  ASSERT_EQ("121", video_section.GetFormats()[1]);
  ASSERT_EQ("126", video_section.GetFormats()[2]);
  ASSERT_EQ("97", video_section.GetFormats()[3]);
  ASSERT_EQ("122", video_section.GetFormats()[4]);
  ASSERT_EQ("123", video_section.GetFormats()[5]);

  // Validate rtpmap
  ASSERT_TRUE(video_attrs.HasAttribute(SdpAttribute::kRtpmapAttribute));
  auto& rtpmaps = video_attrs.GetRtpmap();
  ASSERT_TRUE(rtpmaps.HasEntry("120"));
  ASSERT_TRUE(rtpmaps.HasEntry("121"));
  ASSERT_TRUE(rtpmaps.HasEntry("126"));
  ASSERT_TRUE(rtpmaps.HasEntry("97"));
  ASSERT_TRUE(rtpmaps.HasEntry("122"));
  ASSERT_TRUE(rtpmaps.HasEntry("123"));

  // Validate fmtps
  ASSERT_TRUE(video_attrs.HasAttribute(SdpAttribute::kFmtpAttribute));
  auto& fmtps = video_attrs.GetFmtp().mFmtps;

  ASSERT_EQ(4U, fmtps.size());
  ASSERT_EQ("126", fmtps[0].format);
  ASSERT_EQ("97", fmtps[1].format);
  ASSERT_EQ("120", fmtps[2].format);
  ASSERT_EQ("121", fmtps[3].format);

  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);

  auto offerPairs = mSessionOff->GetNegotiatedTrackPairs();
  ASSERT_EQ(2U, offerPairs.size());
  ASSERT_TRUE(offerPairs[1].mSending);
  ASSERT_TRUE(offerPairs[1].mReceiving);
  ASSERT_TRUE(offerPairs[1].mSending->GetNegotiatedDetails());
  ASSERT_TRUE(offerPairs[1].mReceiving->GetNegotiatedDetails());
  ASSERT_EQ(6U,
      offerPairs[1].mSending->GetNegotiatedDetails()->GetEncoding(0)
      .GetCodecs().size());
  ASSERT_EQ(6U,
      offerPairs[1].mReceiving->GetNegotiatedDetails()->GetEncoding(0)
      .GetCodecs().size());

  auto answerPairs = mSessionAns->GetNegotiatedTrackPairs();
  ASSERT_EQ(2U, answerPairs.size());
  ASSERT_TRUE(answerPairs[1].mSending);
  ASSERT_TRUE(answerPairs[1].mReceiving);
  ASSERT_TRUE(answerPairs[1].mSending->GetNegotiatedDetails());
  ASSERT_TRUE(answerPairs[1].mReceiving->GetNegotiatedDetails());
  ASSERT_EQ(6U,
      answerPairs[1].mSending->GetNegotiatedDetails()->GetEncoding(0)
      .GetCodecs().size());
  ASSERT_EQ(6U,
      answerPairs[1].mReceiving->GetNegotiatedDetails()->GetEncoding(0)
      .GetCodecs().size());
}

TEST_F(JsepSessionTest, ValidateAnsweredCodecParams)
{
  // TODO(bug 1099351): Once fixed, we can allow red in this offer,
  // which will also cause multiple codecs in answer.  For now,
  // red/ulpfec for video are behind a pref to mitigate potential for
  // errors.
  SetCodecEnabled(*mSessionOff, "red", false);
  for (auto i = mSessionAns->Codecs().begin(); i != mSessionAns->Codecs().end();
       ++i) {
    auto* codec = *i;
    if (codec->mName == "H264") {
      JsepVideoCodecDescription* h264 =
          static_cast<JsepVideoCodecDescription*>(codec);
      h264->mProfileLevelId = 0x42a00d;
      // Switch up the pts
      if (h264->mDefaultPt == "126") {
        h264->mDefaultPt = "97";
      } else {
        h264->mDefaultPt = "126";
      }
    }
  }

  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);

  RefPtr<JsepTrack> msta(
      new JsepTrack(SdpMediaSection::kAudio, "offerer_stream", "a1"));
  mSessionOff->AddTrack(msta);
  RefPtr<JsepTrack> mstv1(
      new JsepTrack(SdpMediaSection::kVideo, "offerer_stream", "v1"));
  mSessionOff->AddTrack(mstv1);

  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);

  RefPtr<JsepTrack> msta_ans(
      new JsepTrack(SdpMediaSection::kAudio, "answerer_stream", "a1"));
  mSessionAns->AddTrack(msta);
  RefPtr<JsepTrack> mstv1_ans(
      new JsepTrack(SdpMediaSection::kVideo, "answerer_stream", "v1"));
  mSessionAns->AddTrack(mstv1);

  std::string answer = CreateAnswer();

  UniquePtr<Sdp> outputSdp(Parse(answer));
  ASSERT_TRUE(!!outputSdp);

  ASSERT_EQ(2U, outputSdp->GetMediaSectionCount());
  auto& video_section = outputSdp->GetMediaSection(1);
  ASSERT_EQ(SdpMediaSection::kVideo, video_section.GetMediaType());
  auto& video_attrs = video_section.GetAttributeList();
  ASSERT_EQ(SdpDirectionAttribute::kSendrecv, video_attrs.GetDirection());

  // TODO(bug 1099351): Once fixed, this stuff will need to be updated.
  ASSERT_EQ(1U, video_section.GetFormats().size());
  // ASSERT_EQ(3U, video_section.GetFormats().size());
  ASSERT_EQ("120", video_section.GetFormats()[0]);
  // ASSERT_EQ("121", video_section.GetFormats()[1]);
  // ASSERT_EQ("126", video_section.GetFormats()[2]);
  // ASSERT_EQ("97", video_section.GetFormats()[3]);

  // Validate rtpmap
  ASSERT_TRUE(video_attrs.HasAttribute(SdpAttribute::kRtpmapAttribute));
  auto& rtpmaps = video_attrs.GetRtpmap();
  ASSERT_TRUE(rtpmaps.HasEntry("120"));
  //ASSERT_TRUE(rtpmaps.HasEntry("121"));
  // ASSERT_TRUE(rtpmaps.HasEntry("126"));
  // ASSERT_TRUE(rtpmaps.HasEntry("97"));

  auto& vp8_entry = rtpmaps.GetEntry("120");
  //auto& vp9_entry = rtpmaps.GetEntry("121");
  // auto& h264_1_entry = rtpmaps.GetEntry("126");
  // auto& h264_0_entry = rtpmaps.GetEntry("97");

  ASSERT_EQ("VP8", vp8_entry.name);
  //ASSERT_EQ("VP9", vp9_entry.name);
  // ASSERT_EQ("H264", h264_1_entry.name);
  // ASSERT_EQ("H264", h264_0_entry.name);

  // Validate fmtps
  ASSERT_TRUE(video_attrs.HasAttribute(SdpAttribute::kFmtpAttribute));
  auto& fmtps = video_attrs.GetFmtp().mFmtps;

  ASSERT_EQ(1U, fmtps.size());
  // ASSERT_EQ(3U, fmtps.size());

  // VP8
  ASSERT_EQ("120", fmtps[0].format);
  ASSERT_TRUE(!!fmtps[0].parameters);
  ASSERT_EQ(SdpRtpmapAttributeList::kVP8, fmtps[0].parameters->codec_type);

  auto& parsed_vp8_params =
      *static_cast<const SdpFmtpAttributeList::VP8Parameters*>(
          fmtps[0].parameters.get());

  ASSERT_EQ((uint32_t)12288, parsed_vp8_params.max_fs);
  ASSERT_EQ((uint32_t)60, parsed_vp8_params.max_fr);


  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);

  auto offerPairs = mSessionOff->GetNegotiatedTrackPairs();
  ASSERT_EQ(2U, offerPairs.size());
  ASSERT_TRUE(offerPairs[1].mSending);
  ASSERT_TRUE(offerPairs[1].mReceiving);
  ASSERT_TRUE(offerPairs[1].mSending->GetNegotiatedDetails());
  ASSERT_TRUE(offerPairs[1].mReceiving->GetNegotiatedDetails());
  ASSERT_EQ(1U,
      offerPairs[1].mSending->GetNegotiatedDetails()->GetEncoding(0)
      .GetCodecs().size());
  ASSERT_EQ(1U,
      offerPairs[1].mReceiving->GetNegotiatedDetails()->GetEncoding(0)
      .GetCodecs().size());

  auto answerPairs = mSessionAns->GetNegotiatedTrackPairs();
  ASSERT_EQ(2U, answerPairs.size());
  ASSERT_TRUE(answerPairs[1].mSending);
  ASSERT_TRUE(answerPairs[1].mReceiving);
  ASSERT_TRUE(answerPairs[1].mSending->GetNegotiatedDetails());
  ASSERT_TRUE(answerPairs[1].mReceiving->GetNegotiatedDetails());
  ASSERT_EQ(1U,
      answerPairs[1].mSending->GetNegotiatedDetails()->GetEncoding(0)
      .GetCodecs().size());
  ASSERT_EQ(1U,
      answerPairs[1].mReceiving->GetNegotiatedDetails()->GetEncoding(0)
      .GetCodecs().size());

#if 0
  // H264 packetization mode 1
  ASSERT_EQ("126", fmtps[1].format);
  ASSERT_TRUE(fmtps[1].parameters);
  ASSERT_EQ(SdpRtpmapAttributeList::kH264, fmtps[1].parameters->codec_type);

  auto& parsed_h264_1_params =
    *static_cast<const SdpFmtpAttributeList::H264Parameters*>(
        fmtps[1].parameters.get());

  ASSERT_EQ((uint32_t)0x42a00d, parsed_h264_1_params.profile_level_id);
  ASSERT_TRUE(parsed_h264_1_params.level_asymmetry_allowed);
  ASSERT_EQ(1U, parsed_h264_1_params.packetization_mode);

  // H264 packetization mode 0
  ASSERT_EQ("97", fmtps[2].format);
  ASSERT_TRUE(fmtps[2].parameters);
  ASSERT_EQ(SdpRtpmapAttributeList::kH264, fmtps[2].parameters->codec_type);

  auto& parsed_h264_0_params =
    *static_cast<const SdpFmtpAttributeList::H264Parameters*>(
        fmtps[2].parameters.get());

  ASSERT_EQ((uint32_t)0x42a00d, parsed_h264_0_params.profile_level_id);
  ASSERT_TRUE(parsed_h264_0_params.level_asymmetry_allowed);
  ASSERT_EQ(0U, parsed_h264_0_params.packetization_mode);
#endif
}

TEST_F(JsepSessionTest, OfferWithBundleGroupNoTags)
{
  AddTracks(*mSessionOff, "audio,video");
  AddTracks(*mSessionAns, "audio,video");

  std::string offer = CreateOffer();
  size_t i = offer.find("a=group:BUNDLE");
  offer.insert(i, "a=group:BUNDLE\r\n");

  SetLocalOffer(offer, CHECK_SUCCESS);
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());
}

static void
Replace(const std::string& toReplace,
        const std::string& with,
        std::string* in)
{
  size_t pos = in->find(toReplace);
  ASSERT_NE(std::string::npos, pos);
  in->replace(pos, toReplace.size(), with);
}

static void ReplaceAll(const std::string& toReplace,
                       const std::string& with,
                       std::string* in)
{
  while (in->find(toReplace) != std::string::npos) {
    Replace(toReplace, with, in);
  }
}

static void
GetCodec(JsepSession& session,
         size_t pairIndex,
         sdp::Direction direction,
         size_t encodingIndex,
         size_t codecIndex,
         const JsepCodecDescription** codecOut)
{
  *codecOut = nullptr;
  ASSERT_LT(pairIndex, session.GetNegotiatedTrackPairs().size());
  JsepTrackPair pair(session.GetNegotiatedTrackPairs().front());
  RefPtr<JsepTrack> track(
      (direction == sdp::kSend) ? pair.mSending : pair.mReceiving);
  ASSERT_TRUE(track);
  ASSERT_TRUE(track->GetNegotiatedDetails());
  ASSERT_LT(encodingIndex, track->GetNegotiatedDetails()->GetEncodingCount());
  ASSERT_LT(codecIndex,
      track->GetNegotiatedDetails()->GetEncoding(encodingIndex)
      .GetCodecs().size());
  *codecOut =
      track->GetNegotiatedDetails()->GetEncoding(encodingIndex)
      .GetCodecs()[codecIndex];
}

static void
ForceH264(JsepSession& session, uint32_t profileLevelId)
{
  for (JsepCodecDescription* codec : session.Codecs()) {
    if (codec->mName == "H264") {
      JsepVideoCodecDescription* h264 =
          static_cast<JsepVideoCodecDescription*>(codec);
      h264->mProfileLevelId = profileLevelId;
    } else {
      codec->mEnabled = false;
    }
  }
}

TEST_F(JsepSessionTest, TestH264Negotiation)
{
  ForceH264(*mSessionOff, 0x42e00b);
  ForceH264(*mSessionAns, 0x42e00d);

  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer(CreateOffer());
  SetLocalOffer(offer, CHECK_SUCCESS);

  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());

  SetRemoteAnswer(answer, CHECK_SUCCESS);
  SetLocalAnswer(answer, CHECK_SUCCESS);

  const JsepCodecDescription* offererSendCodec;
  GetCodec(*mSessionOff, 0, sdp::kSend, 0, 0, &offererSendCodec);
  ASSERT_TRUE(offererSendCodec);
  ASSERT_EQ("H264", offererSendCodec->mName);
  const JsepVideoCodecDescription* offererVideoSendCodec(
      static_cast<const JsepVideoCodecDescription*>(offererSendCodec));
  ASSERT_EQ((uint32_t)0x42e00d, offererVideoSendCodec->mProfileLevelId);

  const JsepCodecDescription* offererRecvCodec;
  GetCodec(*mSessionOff, 0, sdp::kRecv, 0, 0, &offererRecvCodec);
  ASSERT_EQ("H264", offererRecvCodec->mName);
  const JsepVideoCodecDescription* offererVideoRecvCodec(
      static_cast<const JsepVideoCodecDescription*>(offererRecvCodec));
  ASSERT_EQ((uint32_t)0x42e00b, offererVideoRecvCodec->mProfileLevelId);

  const JsepCodecDescription* answererSendCodec;
  GetCodec(*mSessionAns, 0, sdp::kSend, 0, 0, &answererSendCodec);
  ASSERT_TRUE(answererSendCodec);
  ASSERT_EQ("H264", answererSendCodec->mName);
  const JsepVideoCodecDescription* answererVideoSendCodec(
      static_cast<const JsepVideoCodecDescription*>(answererSendCodec));
  ASSERT_EQ((uint32_t)0x42e00b, answererVideoSendCodec->mProfileLevelId);

  const JsepCodecDescription* answererRecvCodec;
  GetCodec(*mSessionAns, 0, sdp::kRecv, 0, 0, &answererRecvCodec);
  ASSERT_EQ("H264", answererRecvCodec->mName);
  const JsepVideoCodecDescription* answererVideoRecvCodec(
      static_cast<const JsepVideoCodecDescription*>(answererRecvCodec));
  ASSERT_EQ((uint32_t)0x42e00d, answererVideoRecvCodec->mProfileLevelId);
}

TEST_F(JsepSessionTest, TestH264NegotiationFails)
{
  ForceH264(*mSessionOff, 0x42000b);
  ForceH264(*mSessionAns, 0x42e00d);

  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer(CreateOffer());
  SetLocalOffer(offer, CHECK_SUCCESS);

  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());

  SetRemoteAnswer(answer, CHECK_SUCCESS);
  SetLocalAnswer(answer, CHECK_SUCCESS);

  ASSERT_EQ(0U, mSessionOff->GetNegotiatedTrackPairs().size());
  ASSERT_EQ(0U, mSessionAns->GetNegotiatedTrackPairs().size());
}

TEST_F(JsepSessionTest, TestH264NegotiationOffererDefault)
{
  ForceH264(*mSessionOff, 0x42000d);
  ForceH264(*mSessionAns, 0x42000d);

  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer(CreateOffer());
  SetLocalOffer(offer, CHECK_SUCCESS);

  Replace("profile-level-id=42000d",
          "some-unknown-param=0",
          &offer);

  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());

  SetRemoteAnswer(answer, CHECK_SUCCESS);
  SetLocalAnswer(answer, CHECK_SUCCESS);

  const JsepCodecDescription* answererSendCodec;
  GetCodec(*mSessionAns, 0, sdp::kSend, 0, 0, &answererSendCodec);
  ASSERT_TRUE(answererSendCodec);
  ASSERT_EQ("H264", answererSendCodec->mName);
  const JsepVideoCodecDescription* answererVideoSendCodec(
      static_cast<const JsepVideoCodecDescription*>(answererSendCodec));
  ASSERT_EQ((uint32_t)0x420010, answererVideoSendCodec->mProfileLevelId);
}

TEST_F(JsepSessionTest, TestH264NegotiationOffererNoFmtp)
{
  ForceH264(*mSessionOff, 0x42000d);
  ForceH264(*mSessionAns, 0x42001e);

  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer(CreateOffer());
  SetLocalOffer(offer, CHECK_SUCCESS);

  Replace("a=fmtp", "a=oops", &offer);

  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());

  SetRemoteAnswer(answer, CHECK_SUCCESS);
  SetLocalAnswer(answer, CHECK_SUCCESS);

  const JsepCodecDescription* answererSendCodec;
  GetCodec(*mSessionAns, 0, sdp::kSend, 0, 0, &answererSendCodec);
  ASSERT_TRUE(answererSendCodec);
  ASSERT_EQ("H264", answererSendCodec->mName);
  const JsepVideoCodecDescription* answererVideoSendCodec(
      static_cast<const JsepVideoCodecDescription*>(answererSendCodec));
  ASSERT_EQ((uint32_t)0x420010, answererVideoSendCodec->mProfileLevelId);

  const JsepCodecDescription* answererRecvCodec;
  GetCodec(*mSessionAns, 0, sdp::kRecv, 0, 0, &answererRecvCodec);
  ASSERT_EQ("H264", answererRecvCodec->mName);
  const JsepVideoCodecDescription* answererVideoRecvCodec(
      static_cast<const JsepVideoCodecDescription*>(answererRecvCodec));
  ASSERT_EQ((uint32_t)0x420010, answererVideoRecvCodec->mProfileLevelId);
}

TEST_F(JsepSessionTest, TestH264LevelAsymmetryDisallowedByOffererWithLowLevel)
{
  ForceH264(*mSessionOff, 0x42e00b);
  ForceH264(*mSessionAns, 0x42e00d);

  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer(CreateOffer());
  SetLocalOffer(offer, CHECK_SUCCESS);

  Replace("level-asymmetry-allowed=1",
          "level-asymmetry-allowed=0",
          &offer);

  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());

  SetRemoteAnswer(answer, CHECK_SUCCESS);
  SetLocalAnswer(answer, CHECK_SUCCESS);

  // Offerer doesn't know about the shenanigans we've pulled here, so will
  // behave normally, and we test the normal behavior elsewhere.

  const JsepCodecDescription* answererSendCodec;
  GetCodec(*mSessionAns, 0, sdp::kSend, 0, 0, &answererSendCodec);
  ASSERT_TRUE(answererSendCodec);
  ASSERT_EQ("H264", answererSendCodec->mName);
  const JsepVideoCodecDescription* answererVideoSendCodec(
      static_cast<const JsepVideoCodecDescription*>(answererSendCodec));
  ASSERT_EQ((uint32_t)0x42e00b, answererVideoSendCodec->mProfileLevelId);

  const JsepCodecDescription* answererRecvCodec;
  GetCodec(*mSessionAns, 0, sdp::kRecv, 0, 0, &answererRecvCodec);
  ASSERT_EQ("H264", answererRecvCodec->mName);
  const JsepVideoCodecDescription* answererVideoRecvCodec(
      static_cast<const JsepVideoCodecDescription*>(answererRecvCodec));
  ASSERT_EQ((uint32_t)0x42e00b, answererVideoRecvCodec->mProfileLevelId);
}

TEST_F(JsepSessionTest, TestH264LevelAsymmetryDisallowedByOffererWithHighLevel)
{
  ForceH264(*mSessionOff, 0x42e00d);
  ForceH264(*mSessionAns, 0x42e00b);

  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer(CreateOffer());
  SetLocalOffer(offer, CHECK_SUCCESS);

  Replace("level-asymmetry-allowed=1",
          "level-asymmetry-allowed=0",
          &offer);

  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());

  SetRemoteAnswer(answer, CHECK_SUCCESS);
  SetLocalAnswer(answer, CHECK_SUCCESS);

  // Offerer doesn't know about the shenanigans we've pulled here, so will
  // behave normally, and we test the normal behavior elsewhere.

  const JsepCodecDescription* answererSendCodec;
  GetCodec(*mSessionAns, 0, sdp::kSend, 0, 0, &answererSendCodec);
  ASSERT_TRUE(answererSendCodec);
  ASSERT_EQ("H264", answererSendCodec->mName);
  const JsepVideoCodecDescription* answererVideoSendCodec(
      static_cast<const JsepVideoCodecDescription*>(answererSendCodec));
  ASSERT_EQ((uint32_t)0x42e00b, answererVideoSendCodec->mProfileLevelId);

  const JsepCodecDescription* answererRecvCodec;
  GetCodec(*mSessionAns, 0, sdp::kRecv, 0, 0, &answererRecvCodec);
  ASSERT_EQ("H264", answererRecvCodec->mName);
  const JsepVideoCodecDescription* answererVideoRecvCodec(
      static_cast<const JsepVideoCodecDescription*>(answererRecvCodec));
  ASSERT_EQ((uint32_t)0x42e00b, answererVideoRecvCodec->mProfileLevelId);
}

TEST_F(JsepSessionTest, TestH264LevelAsymmetryDisallowedByAnswererWithLowLevel)
{
  ForceH264(*mSessionOff, 0x42e00d);
  ForceH264(*mSessionAns, 0x42e00b);

  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer(CreateOffer());
  SetLocalOffer(offer, CHECK_SUCCESS);
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());

  Replace("level-asymmetry-allowed=1",
          "level-asymmetry-allowed=0",
          &answer);

  SetRemoteAnswer(answer, CHECK_SUCCESS);
  SetLocalAnswer(answer, CHECK_SUCCESS);

  const JsepCodecDescription* offererSendCodec;
  GetCodec(*mSessionOff, 0, sdp::kSend, 0, 0, &offererSendCodec);
  ASSERT_TRUE(offererSendCodec);
  ASSERT_EQ("H264", offererSendCodec->mName);
  const JsepVideoCodecDescription* offererVideoSendCodec(
      static_cast<const JsepVideoCodecDescription*>(offererSendCodec));
  ASSERT_EQ((uint32_t)0x42e00b, offererVideoSendCodec->mProfileLevelId);

  const JsepCodecDescription* offererRecvCodec;
  GetCodec(*mSessionOff, 0, sdp::kRecv, 0, 0, &offererRecvCodec);
  ASSERT_EQ("H264", offererRecvCodec->mName);
  const JsepVideoCodecDescription* offererVideoRecvCodec(
      static_cast<const JsepVideoCodecDescription*>(offererRecvCodec));
  ASSERT_EQ((uint32_t)0x42e00b, offererVideoRecvCodec->mProfileLevelId);

  // Answerer doesn't know we've pulled these shenanigans, it should act as if
  // it did not set level-asymmetry-required, and we already check that
  // elsewhere
}

TEST_F(JsepSessionTest, TestH264LevelAsymmetryDisallowedByAnswererWithHighLevel)
{
  ForceH264(*mSessionOff, 0x42e00b);
  ForceH264(*mSessionAns, 0x42e00d);

  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer(CreateOffer());
  SetLocalOffer(offer, CHECK_SUCCESS);
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer(CreateAnswer());

  Replace("level-asymmetry-allowed=1",
          "level-asymmetry-allowed=0",
          &answer);

  SetRemoteAnswer(answer, CHECK_SUCCESS);
  SetLocalAnswer(answer, CHECK_SUCCESS);

  const JsepCodecDescription* offererSendCodec;
  GetCodec(*mSessionOff, 0, sdp::kSend, 0, 0, &offererSendCodec);
  ASSERT_TRUE(offererSendCodec);
  ASSERT_EQ("H264", offererSendCodec->mName);
  const JsepVideoCodecDescription* offererVideoSendCodec(
      static_cast<const JsepVideoCodecDescription*>(offererSendCodec));
  ASSERT_EQ((uint32_t)0x42e00b, offererVideoSendCodec->mProfileLevelId);

  const JsepCodecDescription* offererRecvCodec;
  GetCodec(*mSessionOff, 0, sdp::kRecv, 0, 0, &offererRecvCodec);
  ASSERT_EQ("H264", offererRecvCodec->mName);
  const JsepVideoCodecDescription* offererVideoRecvCodec(
      static_cast<const JsepVideoCodecDescription*>(offererRecvCodec));
  ASSERT_EQ((uint32_t)0x42e00b, offererVideoRecvCodec->mProfileLevelId);

  // Answerer doesn't know we've pulled these shenanigans, it should act as if
  // it did not set level-asymmetry-required, and we already check that
  // elsewhere
}

TEST_P(JsepSessionTest, TestRejectMline)
{
  // We need to do this before adding tracks
  types = BuildTypes(GetParam());
  std::sort(types.begin(), types.end());

  switch (types.front()) {
    case SdpMediaSection::kAudio:
      // Sabotage audio
      EnsureNegotiationFailure(types.front(), "opus");
      break;
    case SdpMediaSection::kVideo:
      // Sabotage video
      EnsureNegotiationFailure(types.front(), "H264");
      break;
    case SdpMediaSection::kApplication:
      // Sabotage datachannel
      EnsureNegotiationFailure(types.front(), "webrtc-datachannel");
      break;
    default:
      ASSERT_TRUE(false) << "Unknown media type";
  }

  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  std::string offer = CreateOffer();
  mSessionOff->SetLocalDescription(kJsepSdpOffer, offer);
  mSessionAns->SetRemoteDescription(kJsepSdpOffer, offer);

  std::string answer = CreateAnswer();

  UniquePtr<Sdp> outputSdp(Parse(answer));
  ASSERT_TRUE(!!outputSdp);

  ASSERT_NE(0U, outputSdp->GetMediaSectionCount());
  SdpMediaSection* failed_section = nullptr;

  for (size_t i = 0; i < outputSdp->GetMediaSectionCount(); ++i) {
    if (outputSdp->GetMediaSection(i).GetMediaType() == types.front()) {
      failed_section = &outputSdp->GetMediaSection(i);
    }
  }

  ASSERT_TRUE(failed_section) << "Failed type was entirely absent from SDP";
  auto& failed_attrs = failed_section->GetAttributeList();
  ASSERT_EQ(SdpDirectionAttribute::kInactive, failed_attrs.GetDirection());
  ASSERT_EQ(0U, failed_section->GetPort());

  mSessionAns->SetLocalDescription(kJsepSdpAnswer, answer);
  mSessionOff->SetRemoteDescription(kJsepSdpAnswer, answer);

  size_t numRejected = std::count(types.begin(), types.end(), types.front());
  size_t numAccepted = types.size() - numRejected;

  ASSERT_EQ(numAccepted, mSessionOff->GetNegotiatedTrackPairs().size());
  ASSERT_EQ(numAccepted, mSessionAns->GetNegotiatedTrackPairs().size());

  ASSERT_EQ(types.size(), mSessionOff->GetTransports().size());
  ASSERT_EQ(types.size(), mSessionOff->GetLocalTracks().size());
  ASSERT_EQ(numAccepted, mSessionOff->GetRemoteTracks().size());

  ASSERT_EQ(types.size(), mSessionAns->GetTransports().size());
  ASSERT_EQ(types.size(), mSessionAns->GetLocalTracks().size());
  ASSERT_EQ(types.size(), mSessionAns->GetRemoteTracks().size());
}

TEST_F(JsepSessionTest, CreateOfferNoMlines)
{
  JsepOfferOptions options;
  std::string offer;
  nsresult rv = mSessionOff->CreateOffer(options, &offer);
  ASSERT_NE(NS_OK, rv);
  ASSERT_NE("", mSessionOff->GetLastError());
}

TEST_F(JsepSessionTest, TestIceLite)
{
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer, CHECK_SUCCESS);

  UniquePtr<Sdp> parsedOffer(Parse(offer));
  parsedOffer->GetAttributeList().SetAttribute(
      new SdpFlagAttribute(SdpAttribute::kIceLiteAttribute));

  std::ostringstream os;
  parsedOffer->Serialize(os);
  SetRemoteOffer(os.str(), CHECK_SUCCESS);

  ASSERT_TRUE(mSessionAns->RemoteIsIceLite());
  ASSERT_FALSE(mSessionOff->RemoteIsIceLite());
}

TEST_F(JsepSessionTest, TestIceOptions)
{
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer, CHECK_SUCCESS);
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer, CHECK_SUCCESS);
  SetRemoteAnswer(answer, CHECK_SUCCESS);

  ASSERT_EQ(1U, mSessionOff->GetIceOptions().size());
  ASSERT_EQ("trickle", mSessionOff->GetIceOptions()[0]);

  ASSERT_EQ(1U, mSessionAns->GetIceOptions().size());
  ASSERT_EQ("trickle", mSessionAns->GetIceOptions()[0]);
}

TEST_F(JsepSessionTest, TestExtmap)
{
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  // ssrc-audio-level will be extmap 1 for both
  mSessionOff->AddAudioRtpExtension("foo"); // Default mapping of 2
  mSessionOff->AddAudioRtpExtension("bar"); // Default mapping of 3
  mSessionAns->AddAudioRtpExtension("bar"); // Default mapping of 2
  std::string offer = CreateOffer();
  SetLocalOffer(offer, CHECK_SUCCESS);
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer, CHECK_SUCCESS);
  SetRemoteAnswer(answer, CHECK_SUCCESS);

  UniquePtr<Sdp> parsedOffer(Parse(offer));
  ASSERT_EQ(1U, parsedOffer->GetMediaSectionCount());

  auto& offerMediaAttrs = parsedOffer->GetMediaSection(0).GetAttributeList();
  ASSERT_TRUE(offerMediaAttrs.HasAttribute(SdpAttribute::kExtmapAttribute));
  auto& offerExtmap = offerMediaAttrs.GetExtmap().mExtmaps;
  ASSERT_EQ(3U, offerExtmap.size());
  ASSERT_EQ("urn:ietf:params:rtp-hdrext:ssrc-audio-level",
      offerExtmap[0].extensionname);
  ASSERT_EQ(1U, offerExtmap[0].entry);
  ASSERT_EQ("foo", offerExtmap[1].extensionname);
  ASSERT_EQ(2U, offerExtmap[1].entry);
  ASSERT_EQ("bar", offerExtmap[2].extensionname);
  ASSERT_EQ(3U, offerExtmap[2].entry);

  UniquePtr<Sdp> parsedAnswer(Parse(answer));
  ASSERT_EQ(1U, parsedAnswer->GetMediaSectionCount());

  auto& answerMediaAttrs = parsedAnswer->GetMediaSection(0).GetAttributeList();
  ASSERT_TRUE(answerMediaAttrs.HasAttribute(SdpAttribute::kExtmapAttribute));
  auto& answerExtmap = answerMediaAttrs.GetExtmap().mExtmaps;
  ASSERT_EQ(1U, answerExtmap.size());
  // We ensure that the entry for "bar" matches what was in the offer
  ASSERT_EQ("bar", answerExtmap[0].extensionname);
  ASSERT_EQ(3U, answerExtmap[0].entry);
}

TEST_F(JsepSessionTest, TestExtmapWithDuplicates)
{
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  // ssrc-audio-level will be extmap 1 for both
  mSessionOff->AddAudioRtpExtension("foo"); // Default mapping of 2
  mSessionOff->AddAudioRtpExtension("bar"); // Default mapping of 3
  mSessionOff->AddAudioRtpExtension("bar"); // Should be ignored
  mSessionOff->AddAudioRtpExtension("bar"); // Should be ignored
  mSessionOff->AddAudioRtpExtension("baz"); // Default mapping of 4
  mSessionOff->AddAudioRtpExtension("bar"); // Should be ignored

  std::string offer = CreateOffer();
  UniquePtr<Sdp> parsedOffer(Parse(offer));
  ASSERT_EQ(1U, parsedOffer->GetMediaSectionCount());

  auto& offerMediaAttrs = parsedOffer->GetMediaSection(0).GetAttributeList();
  ASSERT_TRUE(offerMediaAttrs.HasAttribute(SdpAttribute::kExtmapAttribute));
  auto& offerExtmap = offerMediaAttrs.GetExtmap().mExtmaps;
  ASSERT_EQ(4U, offerExtmap.size());
  ASSERT_EQ("urn:ietf:params:rtp-hdrext:ssrc-audio-level",
      offerExtmap[0].extensionname);
  ASSERT_EQ(1U, offerExtmap[0].entry);
  ASSERT_EQ("foo", offerExtmap[1].extensionname);
  ASSERT_EQ(2U, offerExtmap[1].entry);
  ASSERT_EQ("bar", offerExtmap[2].extensionname);
  ASSERT_EQ(3U, offerExtmap[2].entry);
}


TEST_F(JsepSessionTest, TestRtcpFbStar)
{
  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  std::string offer = CreateOffer();

  UniquePtr<Sdp> parsedOffer(Parse(offer));
  auto* rtcpfbs = new SdpRtcpFbAttributeList;
  rtcpfbs->PushEntry("*", SdpRtcpFbAttributeList::kNack);
  parsedOffer->GetMediaSection(0).GetAttributeList().SetAttribute(rtcpfbs);
  offer = parsedOffer->ToString();

  SetLocalOffer(offer, CHECK_SUCCESS);
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer, CHECK_SUCCESS);
  SetRemoteAnswer(answer, CHECK_SUCCESS);

  ASSERT_EQ(1U, mSessionAns->GetRemoteTracks().size());
  RefPtr<JsepTrack> track = mSessionAns->GetRemoteTracks()[0];
  ASSERT_TRUE(track->GetNegotiatedDetails());
  auto* details = track->GetNegotiatedDetails();
  for (const JsepCodecDescription* codec :
       details->GetEncoding(0).GetCodecs()) {
    const JsepVideoCodecDescription* videoCodec =
      static_cast<const JsepVideoCodecDescription*>(codec);
    ASSERT_EQ(1U, videoCodec->mNackFbTypes.size());
    ASSERT_EQ("", videoCodec->mNackFbTypes[0]);
  }
}

TEST_F(JsepSessionTest, TestUniquePayloadTypes)
{
  // The audio payload types will all appear more than once, but the video
  // payload types will be unique.
  AddTracks(*mSessionOff, "audio,audio,video");
  AddTracks(*mSessionAns, "audio,audio,video");

  std::string offer = CreateOffer();
  SetLocalOffer(offer, CHECK_SUCCESS);
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer, CHECK_SUCCESS);
  SetRemoteAnswer(answer, CHECK_SUCCESS);

  auto offerPairs = mSessionOff->GetNegotiatedTrackPairs();
  auto answerPairs = mSessionAns->GetNegotiatedTrackPairs();
  ASSERT_EQ(3U, offerPairs.size());
  ASSERT_EQ(3U, answerPairs.size());

  ASSERT_TRUE(offerPairs[0].mReceiving);
  ASSERT_TRUE(offerPairs[0].mReceiving->GetNegotiatedDetails());
  ASSERT_EQ(0U,
      offerPairs[0].mReceiving->GetNegotiatedDetails()->
      GetUniquePayloadTypes().size());

  ASSERT_TRUE(offerPairs[1].mReceiving);
  ASSERT_TRUE(offerPairs[1].mReceiving->GetNegotiatedDetails());
  ASSERT_EQ(0U,
      offerPairs[1].mReceiving->GetNegotiatedDetails()->
      GetUniquePayloadTypes().size());

  ASSERT_TRUE(offerPairs[2].mReceiving);
  ASSERT_TRUE(offerPairs[2].mReceiving->GetNegotiatedDetails());
  ASSERT_NE(0U,
      offerPairs[2].mReceiving->GetNegotiatedDetails()->
      GetUniquePayloadTypes().size());

  ASSERT_TRUE(answerPairs[0].mReceiving);
  ASSERT_TRUE(answerPairs[0].mReceiving->GetNegotiatedDetails());
  ASSERT_EQ(0U,
      answerPairs[0].mReceiving->GetNegotiatedDetails()->
      GetUniquePayloadTypes().size());

  ASSERT_TRUE(answerPairs[1].mReceiving);
  ASSERT_TRUE(answerPairs[1].mReceiving->GetNegotiatedDetails());
  ASSERT_EQ(0U,
      answerPairs[1].mReceiving->GetNegotiatedDetails()->
      GetUniquePayloadTypes().size());

  ASSERT_TRUE(answerPairs[2].mReceiving);
  ASSERT_TRUE(answerPairs[2].mReceiving->GetNegotiatedDetails());
  ASSERT_NE(0U,
      answerPairs[2].mReceiving->GetNegotiatedDetails()->
      GetUniquePayloadTypes().size());
}

TEST_F(JsepSessionTest, UnknownFingerprintAlgorithm)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");

  std::string offer(CreateOffer());
  SetLocalOffer(offer);
  ReplaceAll("fingerprint:sha", "fingerprint:foo", &offer);
  nsresult rv = mSessionAns->SetRemoteDescription(kJsepSdpOffer, offer);
  ASSERT_NE(NS_OK, rv);
  ASSERT_NE("", mSessionAns->GetLastError());
}

TEST(H264ProfileLevelIdTest, TestLevelComparisons)
{
  ASSERT_LT(JsepVideoCodecDescription::GetSaneH264Level(0x421D0B), // 1b
            JsepVideoCodecDescription::GetSaneH264Level(0x420D0B)); // 1.1
  ASSERT_LT(JsepVideoCodecDescription::GetSaneH264Level(0x420D0A), // 1.0
            JsepVideoCodecDescription::GetSaneH264Level(0x421D0B)); // 1b
  ASSERT_LT(JsepVideoCodecDescription::GetSaneH264Level(0x420D0A), // 1.0
            JsepVideoCodecDescription::GetSaneH264Level(0x420D0B)); // 1.1

  ASSERT_LT(JsepVideoCodecDescription::GetSaneH264Level(0x640009), // 1b
            JsepVideoCodecDescription::GetSaneH264Level(0x64000B)); // 1.1
  ASSERT_LT(JsepVideoCodecDescription::GetSaneH264Level(0x64000A), // 1.0
            JsepVideoCodecDescription::GetSaneH264Level(0x640009)); // 1b
  ASSERT_LT(JsepVideoCodecDescription::GetSaneH264Level(0x64000A), // 1.0
            JsepVideoCodecDescription::GetSaneH264Level(0x64000B)); // 1.1
}

TEST(H264ProfileLevelIdTest, TestLevelSetting)
{
  uint32_t profileLevelId = 0x420D0A;
  JsepVideoCodecDescription::SetSaneH264Level(
      JsepVideoCodecDescription::GetSaneH264Level(0x42100B),
      &profileLevelId);
  ASSERT_EQ((uint32_t)0x421D0B, profileLevelId);

  JsepVideoCodecDescription::SetSaneH264Level(
      JsepVideoCodecDescription::GetSaneH264Level(0x42000A),
      &profileLevelId);
  ASSERT_EQ((uint32_t)0x420D0A, profileLevelId);

  profileLevelId = 0x6E100A;
  JsepVideoCodecDescription::SetSaneH264Level(
      JsepVideoCodecDescription::GetSaneH264Level(0x640009),
      &profileLevelId);
  ASSERT_EQ((uint32_t)0x6E1009, profileLevelId);

  JsepVideoCodecDescription::SetSaneH264Level(
      JsepVideoCodecDescription::GetSaneH264Level(0x64000B),
      &profileLevelId);
  ASSERT_EQ((uint32_t)0x6E100B, profileLevelId);
}

TEST_F(JsepSessionTest, StronglyPreferredCodec)
{
  for (JsepCodecDescription* codec : mSessionAns->Codecs()) {
    if (codec->mName == "H264") {
      codec->mStronglyPreferred = true;
    }
  }

  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "video");
  AddTracks(*mSessionAns, "video");

  OfferAnswer();

  const JsepCodecDescription* codec;
  GetCodec(*mSessionAns, 0, sdp::kSend, 0, 0, &codec);
  ASSERT_TRUE(codec);
  ASSERT_EQ("H264", codec->mName);
  GetCodec(*mSessionAns, 0, sdp::kRecv, 0, 0, &codec);
  ASSERT_TRUE(codec);
  ASSERT_EQ("H264", codec->mName);
}

TEST_F(JsepSessionTest, LowDynamicPayloadType)
{
  SetPayloadTypeNumber(*mSessionOff, "opus", "12");
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");

  OfferAnswer();
  const JsepCodecDescription* codec;
  GetCodec(*mSessionAns, 0, sdp::kSend, 0, 0, &codec);
  ASSERT_TRUE(codec);
  ASSERT_EQ("opus", codec->mName);
  ASSERT_EQ("12", codec->mDefaultPt);
  GetCodec(*mSessionAns, 0, sdp::kRecv, 0, 0, &codec);
  ASSERT_TRUE(codec);
  ASSERT_EQ("opus", codec->mName);
  ASSERT_EQ("12", codec->mDefaultPt);
}

TEST_F(JsepSessionTest, PayloadTypeClash)
{
  // Disable this so mSessionOff doesn't have a duplicate
  SetCodecEnabled(*mSessionOff, "PCMU", false);
  SetPayloadTypeNumber(*mSessionOff, "opus", "0");
  SetPayloadTypeNumber(*mSessionAns, "PCMU", "0");
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");

  OfferAnswer();
  const JsepCodecDescription* codec;
  GetCodec(*mSessionAns, 0, sdp::kSend, 0, 0, &codec);
  ASSERT_TRUE(codec);
  ASSERT_EQ("opus", codec->mName);
  ASSERT_EQ("0", codec->mDefaultPt);
  GetCodec(*mSessionAns, 0, sdp::kRecv, 0, 0, &codec);
  ASSERT_TRUE(codec);
  ASSERT_EQ("opus", codec->mName);
  ASSERT_EQ("0", codec->mDefaultPt);

  // Now, make sure that mSessionAns does not put a=rtpmap:0 PCMU in a reoffer,
  // since pt 0 is taken for opus (the answerer still supports PCMU, and will
  // reoffer it, but it should choose a new payload type for it)
  JsepOfferOptions options;
  std::string reoffer;
  nsresult rv = mSessionAns->CreateOffer(options, &reoffer);
  ASSERT_EQ(NS_OK, rv);
  ASSERT_EQ(std::string::npos, reoffer.find("a=rtpmap:0 PCMU")) << reoffer;
}

TEST_P(JsepSessionTest, TestGlareRollback)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);
  JsepOfferOptions options;

  std::string offer;
  ASSERT_EQ(NS_OK, mSessionAns->CreateOffer(options, &offer));
  ASSERT_EQ(NS_OK,
            mSessionAns->SetLocalDescription(kJsepSdpOffer, offer));
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionAns->GetState());

  ASSERT_EQ(NS_OK, mSessionOff->CreateOffer(options, &offer));
  ASSERT_EQ(NS_OK,
            mSessionOff->SetLocalDescription(kJsepSdpOffer, offer));
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());

  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionAns->SetRemoteDescription(kJsepSdpOffer, offer));
  ASSERT_EQ(NS_OK,
            mSessionAns->SetLocalDescription(kJsepSdpRollback, ""));
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());

  SetRemoteOffer(offer);

  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);
}

TEST_P(JsepSessionTest, TestRejectOfferRollback)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);

  ASSERT_EQ(NS_OK,
            mSessionAns->SetRemoteDescription(kJsepSdpRollback, ""));
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
  ASSERT_EQ(types.size(), mSessionAns->GetRemoteTracksRemoved().size());

  ASSERT_EQ(NS_OK,
            mSessionOff->SetLocalDescription(kJsepSdpRollback, ""));
  ASSERT_EQ(kJsepStateStable, mSessionOff->GetState());

  OfferAnswer();
}

TEST_P(JsepSessionTest, TestInvalidRollback)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionOff->SetLocalDescription(kJsepSdpRollback, ""));
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionOff->SetRemoteDescription(kJsepSdpRollback, ""));

  std::string offer = CreateOffer();
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionOff->SetLocalDescription(kJsepSdpRollback, ""));
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionOff->SetRemoteDescription(kJsepSdpRollback, ""));

  SetLocalOffer(offer);
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionOff->SetRemoteDescription(kJsepSdpRollback, ""));

  SetRemoteOffer(offer);
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionAns->SetLocalDescription(kJsepSdpRollback, ""));

  std::string answer = CreateAnswer();
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionAns->SetLocalDescription(kJsepSdpRollback, ""));

  SetLocalAnswer(answer);
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionAns->SetLocalDescription(kJsepSdpRollback, ""));
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionAns->SetRemoteDescription(kJsepSdpRollback, ""));

  SetRemoteAnswer(answer);
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionOff->SetLocalDescription(kJsepSdpRollback, ""));
  ASSERT_EQ(NS_ERROR_UNEXPECTED,
            mSessionOff->SetRemoteDescription(kJsepSdpRollback, ""));
}

size_t GetActiveTransportCount(const JsepSession& session)
{
  auto transports = session.GetTransports();
  size_t activeTransportCount = 0;
  for (RefPtr<JsepTransport>& transport : transports) {
    activeTransportCount += transport->mComponents;
  }
  return activeTransportCount;
}

TEST_P(JsepSessionTest, TestBalancedBundle)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  mSessionOff->SetBundlePolicy(kBundleBalanced);

  std::string offer = CreateOffer();
  SipccSdpParser parser;
  UniquePtr<Sdp> parsedOffer = parser.Parse(offer);
  ASSERT_TRUE(parsedOffer.get());

  std::map<SdpMediaSection::MediaType, SdpMediaSection*> firstByType;

  for (size_t i = 0; i < parsedOffer->GetMediaSectionCount(); ++i) {
    SdpMediaSection& msection(parsedOffer->GetMediaSection(i));
    bool firstOfType = !firstByType.count(msection.GetMediaType());
    if (firstOfType) {
      firstByType[msection.GetMediaType()] = &msection;
    }
    ASSERT_EQ(!firstOfType,
              msection.GetAttributeList().HasAttribute(
                SdpAttribute::kBundleOnlyAttribute));
  }

  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);

  CheckPairs(*mSessionOff, "Offerer pairs");
  CheckPairs(*mSessionAns, "Answerer pairs");
  EXPECT_EQ(1U, GetActiveTransportCount(*mSessionOff));
  EXPECT_EQ(1U, GetActiveTransportCount(*mSessionAns));
}

TEST_P(JsepSessionTest, TestMaxBundle)
{
  AddTracks(*mSessionOff);
  AddTracks(*mSessionAns);

  mSessionOff->SetBundlePolicy(kBundleMaxBundle);
  OfferAnswer();

  std::string offer = mSessionOff->GetLocalDescription(kJsepDescriptionCurrent);
  SipccSdpParser parser;
  UniquePtr<Sdp> parsedOffer = parser.Parse(offer);
  ASSERT_TRUE(parsedOffer.get());

  ASSERT_FALSE(
      parsedOffer->GetMediaSection(0).GetAttributeList().HasAttribute(
        SdpAttribute::kBundleOnlyAttribute));
  ASSERT_NE(0U, parsedOffer->GetMediaSection(0).GetPort());
  for (size_t i = 1; i < parsedOffer->GetMediaSectionCount(); ++i) {
    ASSERT_TRUE(
        parsedOffer->GetMediaSection(i).GetAttributeList().HasAttribute(
          SdpAttribute::kBundleOnlyAttribute));
    ASSERT_EQ(0U, parsedOffer->GetMediaSection(i).GetPort());
  }


  CheckPairs(*mSessionOff, "Offerer pairs");
  CheckPairs(*mSessionAns, "Answerer pairs");
  EXPECT_EQ(1U, GetActiveTransportCount(*mSessionOff));
  EXPECT_EQ(1U, GetActiveTransportCount(*mSessionAns));
}

TEST_F(JsepSessionTest, TestNonDefaultProtocol)
{
  AddTracks(*mSessionOff, "audio,video,datachannel");
  AddTracks(*mSessionAns, "audio,video,datachannel");

  std::string offer;
  ASSERT_EQ(NS_OK, mSessionOff->CreateOffer(JsepOfferOptions(), &offer));
  offer.replace(offer.find("UDP/TLS/RTP/SAVPF"),
                strlen("UDP/TLS/RTP/SAVPF"),
                "RTP/SAVPF");
  offer.replace(offer.find("UDP/TLS/RTP/SAVPF"),
                strlen("UDP/TLS/RTP/SAVPF"),
                "RTP/SAVPF");
  mSessionOff->SetLocalDescription(kJsepSdpOffer, offer);
  mSessionAns->SetRemoteDescription(kJsepSdpOffer, offer);

  std::string answer;
  mSessionAns->CreateAnswer(JsepAnswerOptions(), &answer);
  UniquePtr<Sdp> parsedAnswer = Parse(answer);
  ASSERT_EQ(3U, parsedAnswer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kRtpSavpf,
            parsedAnswer->GetMediaSection(0).GetProtocol());
  ASSERT_EQ(SdpMediaSection::kRtpSavpf,
            parsedAnswer->GetMediaSection(1).GetProtocol());

  mSessionAns->SetLocalDescription(kJsepSdpAnswer, answer);
  mSessionOff->SetRemoteDescription(kJsepSdpAnswer, answer);

  // Make sure reoffer uses the same protocol as before
  mSessionOff->CreateOffer(JsepOfferOptions(), &offer);
  UniquePtr<Sdp> parsedOffer = Parse(offer);
  ASSERT_EQ(3U, parsedOffer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kRtpSavpf,
            parsedOffer->GetMediaSection(0).GetProtocol());
  ASSERT_EQ(SdpMediaSection::kRtpSavpf,
            parsedOffer->GetMediaSection(1).GetProtocol());

  // Make sure reoffer from other side uses the same protocol as before
  mSessionAns->CreateOffer(JsepOfferOptions(), &offer);
  parsedOffer = Parse(offer);
  ASSERT_EQ(3U, parsedOffer->GetMediaSectionCount());
  ASSERT_EQ(SdpMediaSection::kRtpSavpf,
            parsedOffer->GetMediaSection(0).GetProtocol());
  ASSERT_EQ(SdpMediaSection::kRtpSavpf,
            parsedOffer->GetMediaSection(1).GetProtocol());
}

TEST_F(JsepSessionTest, CreateOfferNoVideoStreamRecvVideo)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));

  CreateOffer(Some(options));
}

TEST_F(JsepSessionTest, CreateOfferNoAudioStreamRecvAudio)
{
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));

  CreateOffer(Some(options));
}

TEST_F(JsepSessionTest, CreateOfferNoVideoStream)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(0U));

  CreateOffer(Some(options));
}

TEST_F(JsepSessionTest, CreateOfferNoAudioStream)
{
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(0U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));

  CreateOffer(Some(options));
}

TEST_F(JsepSessionTest, CreateOfferDontReceiveAudio)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(0U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));

  CreateOffer(Some(options));
}

TEST_F(JsepSessionTest, CreateOfferDontReceiveVideo)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(0U));

  CreateOffer(Some(options));
}

TEST_F(JsepSessionTest, CreateOfferRemoveAudioTrack)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(0U));

  RefPtr<JsepTrack> removedTrack = GetTrackOff(0, types.front());
  ASSERT_TRUE(removedTrack);
  ASSERT_EQ(NS_OK, mSessionOff->RemoveTrack(removedTrack->GetStreamId(),
                                           removedTrack->GetTrackId()));

  CreateOffer(Some(options));
}

TEST_F(JsepSessionTest, CreateOfferDontReceiveAudioRemoveAudioTrack)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(0U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));

  RefPtr<JsepTrack> removedTrack = GetTrackOff(0, types.front());
  ASSERT_TRUE(removedTrack);
  ASSERT_EQ(NS_OK, mSessionOff->RemoveTrack(removedTrack->GetStreamId(),
                                           removedTrack->GetTrackId()));

  CreateOffer(Some(options));
}

TEST_F(JsepSessionTest, CreateOfferDontReceiveVideoRemoveVideoTrack)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(0U));

  RefPtr<JsepTrack> removedTrack = GetTrackOff(0, types.back());
  ASSERT_TRUE(removedTrack);
  ASSERT_EQ(NS_OK, mSessionOff->RemoveTrack(removedTrack->GetStreamId(),
                                           removedTrack->GetTrackId()));

  CreateOffer(Some(options));
}

static const std::string strSampleCandidate =
  "a=candidate:1 1 UDP 2130706431 192.168.2.1 50005 typ host\r\n";

static const unsigned short nSamplelevel = 2;

TEST_F(JsepSessionTest, CreateOfferAddCandidate)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);

  std::string mid;
  bool skipped;
  nsresult rv;
  rv = mSessionOff->AddLocalIceCandidate(strSampleCandidate,
                                        nSamplelevel, &mid, &skipped);
  ASSERT_EQ(NS_OK, rv);
}

TEST_F(JsepSessionTest, AddIceCandidateEarly)
{
  std::string mid;
  bool skipped;
  nsresult rv;
  rv = mSessionOff->AddLocalIceCandidate(strSampleCandidate,
                                        nSamplelevel, &mid, &skipped);

  // This can't succeed without a local description
  ASSERT_NE(NS_OK, rv);
}

TEST_F(JsepSessionTest, OfferAnswerDontAddAudioStreamOnAnswerNoOptions)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");
  AddTracks(*mSessionAns, "video");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));

  CreateOffer(Some(options));
  std::string offer = CreateOffer(Some(options));
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer, CHECK_SUCCESS);
  SetRemoteAnswer(answer, CHECK_SUCCESS);
}

TEST_F(JsepSessionTest, OfferAnswerDontAddVideoStreamOnAnswerNoOptions)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");
  AddTracks(*mSessionAns, "audio");

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));

  CreateOffer(Some(options));
  std::string offer = CreateOffer(Some(options));
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer, CHECK_SUCCESS);
  SetRemoteAnswer(answer, CHECK_SUCCESS);
}

TEST_F(JsepSessionTest, OfferAnswerDontAddAudioVideoStreamsOnAnswerNoOptions)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");
  AddTracks(*mSessionAns);

  JsepOfferOptions options;
  options.mOfferToReceiveAudio = Some(static_cast<size_t>(1U));
  options.mOfferToReceiveVideo = Some(static_cast<size_t>(1U));

  OfferAnswer();
}

TEST_F(JsepSessionTest, OfferAndAnswerWithExtraCodec)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();

  UniquePtr<Sdp> munge = Parse(answer);
  SdpMediaSection& mediaSection = munge->GetMediaSection(0);
  mediaSection.AddCodec("8", "PCMA", 8000, 1);
  std::string sdpString = munge->ToString();

  SetLocalAnswer(sdpString);
  SetRemoteAnswer(answer);
}

TEST_F(JsepSessionTest, AddCandidateInHaveLocalOffer) {
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);

  nsresult rv;
  std::string mid;
  rv = mSessionOff->AddRemoteIceCandidate(strSampleCandidate,
                                         mid, nSamplelevel);
  ASSERT_EQ(NS_ERROR_UNEXPECTED, rv);
}

TEST_F(JsepSessionTest, SetLocalWithoutCreateOffer) {
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");

  std::string offer = CreateOffer();
  nsresult rv = mSessionAns->SetLocalDescription(kJsepSdpOffer, offer);
  ASSERT_EQ(NS_ERROR_UNEXPECTED, rv);
}

TEST_F(JsepSessionTest, SetLocalWithoutCreateAnswer) {
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");

  std::string offer = CreateOffer();
  SetRemoteOffer(offer);
  nsresult rv = mSessionAns->SetLocalDescription(kJsepSdpAnswer, offer);
  ASSERT_EQ(NS_ERROR_UNEXPECTED, rv);
}

// Test for Bug 843595
TEST_F(JsepSessionTest, missingUfrag)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  std::string ufrag = "ice-ufrag";
  std::size_t pos = offer.find(ufrag);
  ASSERT_NE(pos, std::string::npos);
  offer.replace(pos, ufrag.length(), "ice-ufrog");
  nsresult rv = mSessionAns->SetRemoteDescription(kJsepSdpOffer, offer);
  ASSERT_EQ(NS_ERROR_INVALID_ARG, rv);
}

TEST_F(JsepSessionTest, AudioOnlyCalleeNoRtcpMux)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  std::string rtcp_mux = "a=rtcp-mux\r\n";
  std::size_t pos = offer.find(rtcp_mux);
  ASSERT_NE(pos, std::string::npos);
  offer.replace(pos, rtcp_mux.length(), "");
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  pos = answer.find(rtcp_mux);
  ASSERT_EQ(pos, std::string::npos);
}

// This test comes from Bug 810220
TEST_F(JsepSessionTest, AudioOnlyG711Call)
{
  std::string offer =
    "v=0\r\n"
    "o=- 1 1 IN IP4 148.147.200.251\r\n"
    "s=-\r\n"
    "b=AS:64\r\n"
    "t=0 0\r\n"
    "a=fingerprint:sha-256 F3:FA:20:C0:CD:48:C4:5F:02:5F:A5:D3:21:D0:2D:48:"
      "7B:31:60:5C:5A:D8:0D:CD:78:78:6C:6D:CE:CC:0C:67\r\n"
    "m=audio 9000 UDP/TLS/RTP/SAVPF 0 8 126\r\n"
    "c=IN IP4 148.147.200.251\r\n"
    "b=TIAS:64000\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=rtpmap:126 telephone-event/8000\r\n"
    "a=candidate:0 1 udp 2130706432 148.147.200.251 9000 typ host\r\n"
    "a=candidate:0 2 udp 2130706432 148.147.200.251 9005 typ host\r\n"
    "a=ice-ufrag:cYuakxkEKH+RApYE\r\n"
    "a=ice-pwd:bwtpzLZD+3jbu8vQHvEa6Xuq\r\n"
    "a=setup:active\r\n"
    "a=sendrecv\r\n";

  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionAns, "audio");
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer = CreateAnswer();

  // They didn't offer opus, so our answer shouldn't include it.
  ASSERT_EQ(answer.find(" opus/"), std::string::npos);

  // They also didn't offer video or application
  ASSERT_EQ(answer.find("video"), std::string::npos);
  ASSERT_EQ(answer.find("application"), std::string::npos);

  // We should answer with PCMU and telephone-event
  ASSERT_NE(answer.find(" PCMU/8000"), std::string::npos);

  // Double-check the directionality
  ASSERT_NE(answer.find("\r\na=sendrecv"), std::string::npos);

}

TEST_F(JsepSessionTest, AudioOnlyG722Only)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);

  std::string audio = "m=audio 9 UDP/TLS/RTP/SAVPF 109 9 0 8 101\r\n";
  std::size_t pos = offer.find(audio);
  ASSERT_NE(pos, std::string::npos);
  offer.replace(pos, audio.length(),
                "m=audio 65375 UDP/TLS/RTP/SAVPF 9\r\n");
  SetRemoteOffer(offer);

  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  ASSERT_NE(mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
            .find("UDP/TLS/RTP/SAVPF 9\r"),
            std::string::npos);
  ASSERT_NE(mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
            .find("a=rtpmap:9 G722/8000"),
            std::string::npos);
}

TEST_F(JsepSessionTest, AudioOnlyG722Rejected)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);

  std::string audio = "m=audio 9 UDP/TLS/RTP/SAVPF 109 9 0 8 101\r\n";
  std::size_t pos = offer.find(audio);
  ASSERT_NE(pos, std::string::npos);
  offer.replace(pos, audio.length(),
                "m=audio 65375 UDP/TLS/RTP/SAVPF 0 8\r\n");
  SetRemoteOffer(offer);

  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);

  // TODO(bug 814227): Use commented out code instead.
  ASSERT_NE(mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
            .find("UDP/TLS/RTP/SAVPF 0\r"),
            std::string::npos);
  // ASSERT_NE(mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
  //           .find("UDP/TLS/RTP/SAVPF 0 8\r"), std::string::npos);
  ASSERT_NE(mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
            .find("a=rtpmap:0 PCMU/8000"), std::string::npos);
  ASSERT_EQ(mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
            .find("a=rtpmap:109 opus/48000/2"), std::string::npos);
  ASSERT_EQ(mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
            .find("a=rtpmap:9 G722/8000"), std::string::npos);
}

// This test doesn't make sense for bundle
TEST_F(JsepSessionTest, DISABLED_FullCallAudioNoMuxVideoMux)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio,video");
  AddTracks(*mSessionAns, "audio,video");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  std::string rtcp_mux = "a=rtcp-mux\r\n";
  std::size_t pos = offer.find(rtcp_mux);
  ASSERT_NE(pos, std::string::npos);
  offer.replace(pos, rtcp_mux.length(), "");
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();

  size_t match = mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
                                                  .find("\r\na=rtcp-mux");
  ASSERT_NE(match, std::string::npos);
  match = mSessionAns->GetLocalDescription(kJsepDescriptionCurrent)
                                           .find("\r\na=rtcp-mux", match + 1);
  ASSERT_EQ(match, std::string::npos);
}

// Disabled pending resolution of bug 818640.
// Actually, this test is completely broken; you can't just call
// SetRemote/CreateAnswer over and over again.
TEST_F(JsepSessionTest, DISABLED_OfferAllDynamicTypes)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionAns, "audio");

  std::string offer;
  for (int i = 96; i < 128; i++)
  {
    std::stringstream ss;
    ss << i;
    std::cout << "Trying dynamic pt = " << i << std::endl;
    offer =
      "v=0\r\n"
      "o=- 1 1 IN IP4 148.147.200.251\r\n"
      "s=-\r\n"
      "b=AS:64\r\n"
      "t=0 0\r\n"
      "a=fingerprint:sha-256 F3:FA:20:C0:CD:48:C4:5F:02:5F:A5:D3:21:D0:2D:48:"
        "7B:31:60:5C:5A:D8:0D:CD:78:78:6C:6D:CE:CC:0C:67\r\n"
      "m=audio 9000 RTP/AVP " + ss.str() + "\r\n"
      "c=IN IP4 148.147.200.251\r\n"
      "b=TIAS:64000\r\n"
      "a=rtpmap:" + ss.str() +" opus/48000/2\r\n"
      "a=candidate:0 1 udp 2130706432 148.147.200.251 9000 typ host\r\n"
      "a=candidate:0 2 udp 2130706432 148.147.200.251 9005 typ host\r\n"
      "a=ice-ufrag:cYuakxkEKH+RApYE\r\n"
      "a=ice-pwd:bwtpzLZD+3jbu8vQHvEa6Xuq\r\n"
      "a=sendrecv\r\n";

      SetRemoteOffer(offer, CHECK_SUCCESS);
      std::string answer = CreateAnswer();
      ASSERT_NE(answer.find(ss.str() + " opus/"), std::string::npos);
  }
}

TEST_F(JsepSessionTest, ipAddrAnyOffer)
{
  std::string offer =
    "v=0\r\n"
    "o=- 1 1 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "b=AS:64\r\n"
    "t=0 0\r\n"
    "a=fingerprint:sha-256 F3:FA:20:C0:CD:48:C4:5F:02:5F:A5:D3:21:D0:2D:48:"
      "7B:31:60:5C:5A:D8:0D:CD:78:78:6C:6D:CE:CC:0C:67\r\n"
    "m=audio 9000 UDP/TLS/RTP/SAVPF 99\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:99 opus/48000/2\r\n"
    "a=ice-ufrag:cYuakxkEKH+RApYE\r\n"
    "a=ice-pwd:bwtpzLZD+3jbu8vQHvEa6Xuq\r\n"
    "a=setup:active\r\n"
    "a=sendrecv\r\n";

  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionAns, "audio");
  SetRemoteOffer(offer, CHECK_SUCCESS);
  std::string answer = CreateAnswer();

  ASSERT_NE(answer.find("a=sendrecv"), std::string::npos);
}

static void CreateSDPForBigOTests(std::string& offer, const std::string& number) {
  offer =
    "v=0\r\n"
    "o=- ";
  offer += number;
  offer += " ";
  offer += number;
  offer += " IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "b=AS:64\r\n"
    "t=0 0\r\n"
    "a=fingerprint:sha-256 F3:FA:20:C0:CD:48:C4:5F:02:5F:A5:D3:21:D0:2D:48:"
      "7B:31:60:5C:5A:D8:0D:CD:78:78:6C:6D:CE:CC:0C:67\r\n"
    "m=audio 9000 RTP/AVP 99\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:99 opus/48000/2\r\n"
    "a=ice-ufrag:cYuakxkEKH+RApYE\r\n"
    "a=ice-pwd:bwtpzLZD+3jbu8vQHvEa6Xuq\r\n"
    "a=setup:active\r\n"
    "a=sendrecv\r\n";
}

TEST_F(JsepSessionTest, BigOValues)
{
  std::string offer;

  CreateSDPForBigOTests(offer, "12345678901234567");

  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionAns, "audio");
  SetRemoteOffer(offer, CHECK_SUCCESS);
}

TEST_F(JsepSessionTest, BigOValuesExtraChars)
{
  std::string offer;

  CreateSDPForBigOTests(offer, "12345678901234567FOOBAR");

  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionAns, "audio");
  // The signaling state will remain "stable" because the unparsable
  // SDP leads to a failure in SetRemoteDescription.
  SetRemoteOffer(offer, NO_CHECKS);
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
}

TEST_F(JsepSessionTest, BigOValuesTooBig)
{
  std::string offer;

  CreateSDPForBigOTests(offer, "18446744073709551615");
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionAns, "audio");

  // The signaling state will remain "stable" because the unparsable
  // SDP leads to a failure in SetRemoteDescription.
  SetRemoteOffer(offer, NO_CHECKS);
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
}


TEST_F(JsepSessionTest, SetLocalAnswerInStable)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  std::string offer = CreateOffer();

  // The signaling state will remain "stable" because the
  // SetLocalDescription call fails.
  SetLocalAnswer(offer, NO_CHECKS);
  ASSERT_EQ(kJsepStateStable, mSessionOff->GetState());
}

TEST_F(JsepSessionTest, SetRemoteAnswerInStable)
{
  const std::string answer =
    "v=0\r\n"
    "o=Mozilla-SIPUA 4949 0 IN IP4 10.86.255.143\r\n"
    "s=SIP Call\r\n"
    "t=0 0\r\n"
    "a=ice-ufrag:qkEP\r\n"
    "a=ice-pwd:ed6f9GuHjLcoCN6sC/Eh7fVl\r\n"
    "m=audio 16384 RTP/AVP 0 8 9 101\r\n"
    "c=IN IP4 10.86.255.143\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=rtpmap:9 G722/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "a=fmtp:101 0-15\r\n"
    "a=sendrecv\r\n"
    "a=candidate:1 1 UDP 2130706431 192.168.2.1 50005 typ host\r\n"
    "a=candidate:2 2 UDP 2130706431 192.168.2.2 50006 typ host\r\n"
    "m=video 1024 RTP/AVP 97\r\n"
    "c=IN IP4 10.86.255.143\r\n"
    "a=rtpmap:120 VP8/90000\r\n"
    "a=fmtp:97 profile-level-id=42E00C\r\n"
    "a=sendrecv\r\n"
    "a=candidate:1 1 UDP 2130706431 192.168.2.3 50007 typ host\r\n"
    "a=candidate:2 2 UDP 2130706431 192.168.2.4 50008 typ host\r\n";

  // The signaling state will remain "stable" because the
  // SetRemoteDescription call fails.
  nsresult rv = mSessionOff->SetRemoteDescription(kJsepSdpAnswer, answer);
  ASSERT_NE(NS_OK, rv);
  ASSERT_EQ(kJsepStateStable, mSessionOff->GetState());
}

TEST_F(JsepSessionTest, SetLocalAnswerInHaveLocalOffer)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  std::string offer = CreateOffer();

  SetLocalOffer(offer);
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());

  // The signaling state will remain "have-local-offer" because the
  // SetLocalDescription call fails.
  nsresult rv = mSessionOff->SetLocalDescription(kJsepSdpAnswer, offer);
  ASSERT_NE(NS_OK, rv);
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());
}

TEST_F(JsepSessionTest, SetRemoteOfferInHaveLocalOffer)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  std::string offer = CreateOffer();

  SetLocalOffer(offer);
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());

  // The signaling state will remain "have-local-offer" because the
  // SetRemoteDescription call fails.
  nsresult rv = mSessionOff->SetRemoteDescription(kJsepSdpOffer, offer);
  ASSERT_NE(NS_OK, rv);
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());
}

TEST_F(JsepSessionTest, SetLocalOfferInHaveRemoteOffer)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  std::string offer = CreateOffer();

  SetRemoteOffer(offer);
  ASSERT_EQ(kJsepStateHaveRemoteOffer, mSessionAns->GetState());

  // The signaling state will remain "have-remote-offer" because the
  // SetLocalDescription call fails.
  nsresult rv = mSessionAns->SetLocalDescription(kJsepSdpOffer, offer);
  ASSERT_NE(NS_OK, rv);
  ASSERT_EQ(kJsepStateHaveRemoteOffer, mSessionAns->GetState());
}

TEST_F(JsepSessionTest, SetRemoteAnswerInHaveRemoteOffer)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  std::string offer = CreateOffer();

  SetRemoteOffer(offer);
  ASSERT_EQ(kJsepStateHaveRemoteOffer, mSessionAns->GetState());

  // The signaling state will remain "have-remote-offer" because the
  // SetRemoteDescription call fails.
  nsresult rv = mSessionAns->SetRemoteDescription(kJsepSdpAnswer, offer);
  ASSERT_NE(NS_OK, rv);

  ASSERT_EQ(kJsepStateHaveRemoteOffer, mSessionAns->GetState());
}

TEST_F(JsepSessionTest, RtcpFbInOffer)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");
  std::string offer = CreateOffer();

  std::map<std::string, bool> expected;
  expected["nack"] = false;
  expected["nack pli"] = false;
  expected["ccm fir"] = false;

  size_t prev = 0;
  size_t found = 0;
  for(;;) {
    found = offer.find('\n', found + 1);
    if (found == std::string::npos)
      break;

    std::string line = offer.substr(prev, (found - prev));

    // ensure no other rtcp-fb values are present
    if (line.find("a=rtcp-fb:") != std::string::npos) {
      size_t space = line.find(' ');
      //strip trailing \r\n
      std::string value = line.substr(space + 1, line.length() - space - 2);
      std::map<std::string, bool>::iterator entry = expected.find(value);
      ASSERT_NE(entry, expected.end());
      entry->second = true;
    }

    prev = found + 1;
  }

  // ensure all values are present
  for (std::map<std::string, bool>::iterator it = expected.begin(); it != expected.end(); ++it) {
    ASSERT_EQ(it->second, true);
  }
}

// In this test we will change the offer SDP's a=setup value
// from actpass to passive. This will force the answer to do active.
TEST_F(JsepSessionTest, AudioCallForceDtlsRoles)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();

  std::string actpass = "\r\na=setup:actpass";
  size_t match = offer.find(actpass);
  ASSERT_NE(match, std::string::npos);
  offer.replace(match, actpass.length(), "\r\na=setup:passive");

  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  ASSERT_EQ(kJsepStateHaveRemoteOffer, mSessionAns->GetState());
  std::string answer = CreateAnswer();
  match = answer.find("\r\na=setup:active");
  ASSERT_NE(match, std::string::npos);

  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
}

// In this test we will change the offer SDP's a=setup value
// from actpass to active. This will force the answer to do passive.
TEST_F(JsepSessionTest, AudioCallReverseDtlsRoles)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();

  std::string actpass = "\r\na=setup:actpass";
  size_t match = offer.find(actpass);
  ASSERT_NE(match, std::string::npos);
  offer.replace(match, actpass.length(), "\r\na=setup:active");

  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  ASSERT_EQ(kJsepStateHaveRemoteOffer, mSessionAns->GetState());
  std::string answer = CreateAnswer();
  match = answer.find("\r\na=setup:passive");
  ASSERT_NE(match, std::string::npos);

  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
}

// In this test we will change the answer SDP's a=setup value
// from active to passive.  This will make both sides do
// active and should not connect.
TEST_F(JsepSessionTest, AudioCallMismatchDtlsRoles)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();

  std::string actpass = "\r\na=setup:actpass";
  size_t match = offer.find(actpass);
  ASSERT_NE(match, std::string::npos);
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  ASSERT_EQ(kJsepStateHaveRemoteOffer, mSessionAns->GetState());
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);

  std::string active = "\r\na=setup:active";
  match = answer.find(active);
  ASSERT_NE(match, std::string::npos);
  answer.replace(match, active.length(), "\r\na=setup:passive");
  SetRemoteAnswer(answer);

  // This is as good as it gets in a JSEP test (w/o starting DTLS)
  ASSERT_EQ(JsepDtlsTransport::kJsepDtlsClient,
      mSessionOff->GetTransports()[0]->mDtls->GetRole());
  ASSERT_EQ(JsepDtlsTransport::kJsepDtlsClient,
      mSessionAns->GetTransports()[0]->mDtls->GetRole());
}

// Verify that missing a=setup in offer gets rejected
TEST_F(JsepSessionTest, AudioCallOffererNoSetup)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);

  std::string actpass = "\r\na=setup:actpass";
  size_t match = offer.find(actpass);
  ASSERT_NE(match, std::string::npos);
  offer.replace(match, actpass.length(), "");

  // The signaling state will remain "stable" because the unparsable
  // SDP leads to a failure in SetRemoteDescription.
  SetRemoteOffer(offer, NO_CHECKS);
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());
}

// In this test we will change the answer SDP to remove the
// a=setup line, which results in active being assumed.
TEST_F(JsepSessionTest, AudioCallAnswerNoSetup)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  size_t match = offer.find("\r\na=setup:actpass");
  ASSERT_NE(match, std::string::npos);

  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  ASSERT_EQ(kJsepStateHaveRemoteOffer, mSessionAns->GetState());
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);

  std::string active = "\r\na=setup:active";
  match = answer.find(active);
  ASSERT_NE(match, std::string::npos);
  answer.replace(match, active.length(), "");
  SetRemoteAnswer(answer);
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());

  // This is as good as it gets in a JSEP test (w/o starting DTLS)
  ASSERT_EQ(JsepDtlsTransport::kJsepDtlsServer,
      mSessionOff->GetTransports()[0]->mDtls->GetRole());
  ASSERT_EQ(JsepDtlsTransport::kJsepDtlsClient,
      mSessionAns->GetTransports()[0]->mDtls->GetRole());
}

// Verify that 'holdconn' gets rejected
TEST_F(JsepSessionTest, AudioCallDtlsRoleHoldconn)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);

  std::string actpass = "\r\na=setup:actpass";
  size_t match = offer.find(actpass);
  ASSERT_NE(match, std::string::npos);
  offer.replace(match, actpass.length(), "\r\na=setup:holdconn");

  // The signaling state will remain "stable" because the unparsable
  // SDP leads to a failure in SetRemoteDescription.
  SetRemoteOffer(offer, NO_CHECKS);
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());
}

// Verify that 'actpass' in answer gets rejected
TEST_F(JsepSessionTest, AudioCallAnswererUsesActpass)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  SetLocalAnswer(answer);

  std::string active = "\r\na=setup:active";
  size_t match = answer.find(active);
  ASSERT_NE(match, std::string::npos);
  answer.replace(match, active.length(), "\r\na=setup:actpass");

  // The signaling state will remain "stable" because the unparsable
  // SDP leads to a failure in SetRemoteDescription.
  SetRemoteAnswer(answer, NO_CHECKS);
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());
}

// Disabled: See Bug 1329028
TEST_F(JsepSessionTest, DISABLED_AudioCallOffererAttemptsSetupRoleSwitch)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");

  OfferAnswer();

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  std::string reoffer = CreateOffer();
  SetLocalOffer(reoffer);

  std::string actpass = "\r\na=setup:actpass";
  size_t match = reoffer.find(actpass);
  ASSERT_NE(match, std::string::npos);
  reoffer.replace(match, actpass.length(), "\r\na=setup:active");

  // This is expected to fail.
  SetRemoteOffer(reoffer, NO_CHECKS);
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
}

// Disabled: See Bug 1329028
TEST_F(JsepSessionTest, DISABLED_AudioCallAnswererAttemptsSetupRoleSwitch)
{
  types.push_back(SdpMediaSection::kAudio);
  AddTracks(*mSessionOff, "audio");
  AddTracks(*mSessionAns, "audio");

  OfferAnswer();

  ValidateSetupAttribute(*mSessionOff, SdpSetupAttribute::kActpass);
  ValidateSetupAttribute(*mSessionAns, SdpSetupAttribute::kActive);

  std::string reoffer = CreateOffer();
  SetLocalOffer(reoffer);
  SetRemoteOffer(reoffer);

  std::string reanswer = CreateAnswer();
  SetLocalAnswer(reanswer);

  std::string actpass = "\r\na=setup:active";
  size_t match = reanswer.find(actpass);
  ASSERT_NE(match, std::string::npos);
  reanswer.replace(match, actpass.length(), "\r\na=setup:passive");

  // This is expected to fail.
  SetRemoteAnswer(reanswer, NO_CHECKS);
  ASSERT_EQ(kJsepStateHaveLocalOffer, mSessionOff->GetState());
  ASSERT_EQ(kJsepStateStable, mSessionAns->GetState());
}

// Remove H.264 P1 and VP8 from offer, check answer negotiates H.264 P0
TEST_F(JsepSessionTest, OfferWithOnlyH264P0)
{
  for (JsepCodecDescription* codec : mSessionOff->Codecs()) {
    if (codec->mName != "H264" || codec->mDefaultPt == "126") {
      codec->mEnabled = false;
    }
  }

  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");
  AddTracks(*mSessionAns, "audio,video");
  std::string offer = CreateOffer();

  ASSERT_EQ(offer.find("a=rtpmap:126 H264/90000"), std::string::npos);
  ASSERT_EQ(offer.find("a=rtpmap:120 VP8/90000"), std::string::npos);

  SetLocalOffer(offer);
  SetRemoteOffer(offer);
  std::string answer = CreateAnswer();
  size_t match = answer.find("\r\na=setup:active");
  ASSERT_NE(match, std::string::npos);

  // validate answer SDP
  ASSERT_NE(answer.find("a=rtpmap:97 H264/90000"), std::string::npos);
  ASSERT_NE(answer.find("a=rtcp-fb:97 nack"), std::string::npos);
  ASSERT_NE(answer.find("a=rtcp-fb:97 nack pli"), std::string::npos);
  ASSERT_NE(answer.find("a=rtcp-fb:97 ccm fir"), std::string::npos);
  // Ensure VP8 and P1 removed
  ASSERT_EQ(answer.find("a=rtpmap:126 H264/90000"), std::string::npos);
  ASSERT_EQ(answer.find("a=rtpmap:120 VP8/90000"), std::string::npos);
  ASSERT_EQ(answer.find("a=rtcp-fb:120"), std::string::npos);
  ASSERT_EQ(answer.find("a=rtcp-fb:126"), std::string::npos);
}

// Test negotiating an answer which has only H.264 P1
// Which means replace VP8 with H.264 P1 in answer
TEST_F(JsepSessionTest, AnswerWithoutVP8)
{
  types.push_back(SdpMediaSection::kAudio);
  types.push_back(SdpMediaSection::kVideo);
  AddTracks(*mSessionOff, "audio,video");
  AddTracks(*mSessionAns, "audio,video");
  std::string offer = CreateOffer();
  SetLocalOffer(offer);
  SetRemoteOffer(offer);

  for (JsepCodecDescription* codec : mSessionOff->Codecs()) {
    if (codec->mName != "H264" || codec->mDefaultPt == "126") {
      codec->mEnabled = false;
    }
  }

  std::string answer = CreateAnswer();

  SetLocalAnswer(answer);
  SetRemoteAnswer(answer);
}

} // namespace mozilla
