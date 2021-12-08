/*********************************************************************************************************************
Copyright (c) 2020 RoboSense
All rights reserved

By downloading, copying, installing or using the software you agree to this license. If you do not agree to this
license, do not download, install, copy or use the software.

License Agreement
For RoboSense LiDAR SDK Library
(3-clause BSD License)

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the names of the RoboSense, nor Suteng Innovation Technology, nor the names of other contributors may be used
to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*********************************************************************************************************************/

#include <rs_driver/driver/decoder/decoder.hpp>
#include <rs_driver/driver/decoder/packet_traverser.hpp>

namespace robosense
{
namespace lidar
{

#pragma pack(push, 1)
typedef struct
{
  uint8_t id[2];
  uint16_t azimuth;
  RSChannel channels[32];
} RS32MsopBlock;

typedef struct
{
  RSMsopHeaderV1 header;
  RS32MsopBlock blocks[12];
  unsigned int index;
  uint16_t tail;
} RS32MsopPkt;

typedef struct
{
  uint8_t id[8];
  uint16_t rpm;
  RSEthNet eth;
  RSFOV fov;
  uint16_t reserved0;
  uint16_t phase_lock_angle;
  RSVersion version;
  uint8_t reserved_1[242];
  RSSn sn;
  uint16_t zero_cali;
  uint8_t return_mode;
  uint16_t sw_ver;
  RSTimestampYMD timestamp;
  RSStatus status;
  uint8_t reserved_2[5];
  RSDiagno diagno;
  uint8_t gprmc[86];
  RSCalibrationAngle ver_angle_cali[32];
  RSCalibrationAngle hori_angle_cali[32];
  uint8_t reserved_3[586];
  uint16_t tail;
} RS32DifopPkt;

#pragma pack(pop)

template <typename T_PointCloud>
class DecoderRS32 : public Decoder<T_PointCloud>
{
public:

  virtual void processDifopPkt(const uint8_t* pkt, size_t size);
  virtual void decodeMsopPkt(const uint8_t* pkt, size_t size);
  virtual uint64_t usecToDelay() {return 0;}
  virtual ~DecoderRS32() = default;

  explicit DecoderRS32(const RSDecoderParam& param,
      const std::function<void(const Error&)>& excb);

  static RSDecoderConstParam getConstParam()
  {
    RSDecoderConstParam param = 
    {
        1248 
      , 1248
      , 8
      , 8
      , {0x55, 0xAA, 0x05, 0x0A, 0x5A, 0xA5, 0x50, 0xA0} // msop id
      , {0xA5, 0xFF, 0x00, 0x5A, 0x11, 0x11, 0x55, 0x55} // difop id
      , {0xFF, 0xEE} // block id
      , 12 // blocks per packet
      , 32 // channels per block
      , 0.005 // distance resolution

      // firing_ts
      , { 0.00,  2.88,  5.76,  8.64, 11.52, 14.40, 17.28, 20.16, 
        23.04, 25.92, 28.80, 31.68, 34.56, 37.44, 40.32, 44.64,
        1.44,  4.32,  7.20, 10.08, 12.96, 15.84, 18.72, 21.60,
        24.48, 27.36, 30.24, 33.12, 36.00, 38.88, 41.76, 46.08}
      , 55.52

        // lens center
      , 0.03997 // RX
      , -0.01087 // RY
      , 0 // RZ
    };

    return param;
  }

  RSEchoMode getEchoMode(uint8_t mode)
  {
    switch (mode)
    {
      case 0x00:
        return RSEchoMode::ECHO_DUAL;
      case 0x01:
      case 0x02:
      default:
        return RSEchoMode::ECHO_SINGLE;
    }
  }

};

template <typename T_PointCloud>
inline DecoderRS32<T_PointCloud>::DecoderRS32(const RSDecoderParam& param,
      const std::function<void(const Error&)>& excb)
  : Decoder<T_PointCloud>(param, excb, getConstParam())
{
}

template <typename T_PointCloud>
inline void DecoderRS32<T_PointCloud>::processDifopPkt(const uint8_t* packet, size_t size)
{
  const RS32DifopPkt& pkt = *(const RS32DifopPkt*)(packet);

  hexdump (packet, size, "difop");

  if (size != this->const_param_.DIFOP_LEN)
  {
     this->excb_(Error(ERRCODE_WRONGPKTLENGTH));
  }

  if (memcmp(this->const_param_.DIFOP_ID, pkt.id, 8) != 0)
  {
      this->excb_(Error(ERRCODE_WRONGPKTHEADER));
  }

  this->echo_mode_ = getEchoMode (pkt.return_mode);

  this->template decodeDifopCommon<RS32DifopPkt>(pkt);

  if (!this->difop_ready_)
  {
    this->chan_angles_.loadFromDifop(pkt.ver_angle_cali, pkt.hori_angle_cali, 32);
  }
}

template <typename T_PointCloud>
inline void DecoderRS32<T_PointCloud>::decodeMsopPkt(const uint8_t* packet, size_t size)
{
  const RS32MsopPkt& pkt = *(const RS32MsopPkt*)(packet);

  this->temperature_ = calcTemp(&(pkt.header.temp));

  double pkt_ts = 0;
  if (this->param_.use_lidar_clock)
  {
    pkt_ts = calcTimeYMD(&pkt.header.timestamp);
  }
  else
  {
    pkt_ts = calcTimeHost();
  }

  SingleReturnPacketTraverser<RS32MsopPkt> traverser(this->const_param_, pkt, pkt_ts);
  
  for (; !traverser.isLast(); traverser.toNext())
  {
    uint16_t blk, chan;
    int16_t angle_horiz;
    double chan_ts;
    traverser.get (blk, chan, angle_horiz, chan_ts);

    const RS32MsopBlock& block = pkt.blocks[blk];
    const RSChannel& channel = block.channels[chan];

    if (memcmp(this->const_param_.BLOCK_ID, block.id, 2) != 0)
    {
      this->excb_(Error(ERRCODE_WRONGPKTHEADER));
      //return RSDecoderResult::WRONG_PKT_HEADER;
    }

    float distance = ntohs(channel.distance) * this->const_param_.DIS_RESOLUTION;
    uint8_t intensity = channel.intensity;
    int16_t angle_vert = this->chan_angles_.vertAdjust(chan);
    int16_t angle_horiz_final = this->chan_angles_.horizAdjust(chan, angle_horiz);

    if (this->distance_block_.in(distance) && this->scan_block_.in(angle_horiz_final))
    {
      float x =  distance * COS(angle_vert) * COS(angle_horiz_final) + this->const_param_.RX * COS(angle_horiz);
      float y = -distance * COS(angle_vert) * SIN(angle_horiz_final) - this->const_param_.RX * SIN(angle_horiz);
      float z =  distance * SIN(angle_vert) + this->const_param_.RZ;

      typename T_PointCloud::PointT point;
      setX(point, x);
      setY(point, y);
      setZ(point, z);
      setIntensity(point, intensity);

      setRing(point, this->chan_angles_.toUserChan(chan));
      setTimestamp(point, chan_ts);
      this->point_cloud_->points.emplace_back(point);
    }
    else if (!this->param_.dense_points)
    {
      typename T_PointCloud::PointT point;
      setX(point, NAN);
      setY(point, NAN);
      setZ(point, NAN);
      setIntensity(point, 0);
      setRing(point, this->chan_angles_.toUserChan(chan));
      setTimestamp(point, chan_ts);
      this->point_cloud_->points.emplace_back(point);
    }
  }
}

}  // namespace lidar
}  // namespace robosense
