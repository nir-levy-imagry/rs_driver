/******************************************************************************
 * Copyright 2020 RoboSense All rights reserved.
 * Suteng Innovation Technology Co., Ltd. www.robosense.ai

 * This software is provided to you directly by RoboSense and might
 * only be used to access RoboSense LiDAR. Any compilation,
 * modification, exploration, reproduction and redistribution are
 * restricted without RoboSense's prior consent.

 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL ROBOSENSE BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/
#include <rs_driver/driver/decoder/decoder_base.hpp>
namespace robosense
{
    namespace lidar
    {
#define RS32_CHANNELS_PER_BLOCK (32)
#define RS32_BLOCKS_PER_PKT (12)
#define RS32_POINTS_CHANNEL_PER_SECOND (18000)
#define RS32_BLOCKS_CHANNEL_PER_PKT (12)
#define RS32_MSOP_ID (0xA050A55A0A05AA55)
#define RS32_BLOCK_ID (0xEEFF)
#define RS32_DIFOP_ID (0x555511115A00FFA5)
#define RS32_CHANNEL_TOFFSET (3)
#define RS32_FIRING_TDURATION (50)

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

        typedef struct
        {
            uint16_t id;
            uint16_t azimuth;
            RS_Channel channels[RS32_CHANNELS_PER_BLOCK];
        }
#ifdef __GNUC__
        __attribute__((packed))
#endif
        RS32_MsopBlock;

        typedef struct
        {
            RS_MsopHeader header;
            RS32_MsopBlock blocks[RS32_BLOCKS_PER_PKT];
            uint32_t index;
            uint16_t tail;
        }
#ifdef __GNUC__
        __attribute__((packed))
#endif
        RS32_MsopPkt;

        typedef struct
        {
            uint8_t reserved[240];
            uint8_t coef;
            uint8_t ver;
        }
#ifdef __GNUC__
        __attribute__((packed))
#endif
        RS32_Intensity;

        typedef struct
        {
            uint64_t id;
            uint16_t rpm;
            RS_EthNet eth;
            RS_ROV fov;
            uint16_t reserved0;
            uint16_t phase_lock_angle;
            RS_Version version;
            RS32_Intensity intensity;
            RS_SN sn;
            uint16_t zero_cali;
            uint8_t return_mode;
            uint16_t sw_ver;
            RS_Timestamp timestamp;
            RS_Status status;
            uint8_t reserved1[11];
            RS_Diagno diagno;
            uint8_t gprmc[86];
            uint8_t pitch_cali[96];
            uint8_t yaw_cali[96];
            uint8_t reserved2[586];
            uint16_t tail;
        }
#ifdef __GNUC__
        __attribute__((packed))
#endif
        RS32_DifopPkt;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

        template <typename vpoint>
        class Decoder32 : public DecoderBase<vpoint>
        {
        public:
            Decoder32(const RSDecoder_Param &param);
            int32_t decodeDifopPkt(const uint8_t *pkt);
            int32_t decodeMsopPkt(const uint8_t *pkt, std::vector<vpoint> &vec, int &height);
            double getLidarTime(const uint8_t *pkt);
            void loadCalibrationFile(const std::string &angle_path);
        };

        template <typename vpoint>
        Decoder32<vpoint>::Decoder32(const RSDecoder_Param &param) : DecoderBase<vpoint>(param)
        {
            this->Rx_ = 0.03997;
            this->Ry_ = -0.01087;
            this->Rz_ = 0;
            this->channel_num_ = 32;
            if (this->max_distance_ > 200.0f || this->max_distance_ < 0.4f)
            {
                this->max_distance_ = 200.0f;
            }
            if (this->min_distance_ > 200.0f || this->min_distance_ > this->max_distance_)
            {
                this->min_distance_ = 0.4f;
            }
            //    rs_print(RS_INFO, "[RS32] Constructor.");
        }

        template <typename vpoint>
        double Decoder32<vpoint>::getLidarTime(const uint8_t *pkt)
        {
            RS32_MsopPkt *mpkt_ptr = (RS32_MsopPkt *)pkt;
            std::tm stm;
            memset(&stm, 0, sizeof(stm));
            stm.tm_year = mpkt_ptr->header.timestamp.year + 100;
            stm.tm_mon = mpkt_ptr->header.timestamp.month - 1;
            stm.tm_mday = mpkt_ptr->header.timestamp.day;
            stm.tm_hour = mpkt_ptr->header.timestamp.hour;
            stm.tm_min = mpkt_ptr->header.timestamp.minute;
            stm.tm_sec = mpkt_ptr->header.timestamp.second;
            return std::mktime(&stm) + (double)RS_SWAP_SHORT(mpkt_ptr->header.timestamp.ms) / 1000.0 + (double)RS_SWAP_SHORT(mpkt_ptr->header.timestamp.us) / 1000000.0;
        }

        template <typename vpoint>
        int Decoder32<vpoint>::decodeMsopPkt(const uint8_t *pkt, std::vector<vpoint> &vec, int &height)
        {
            height = 32;
            RS32_MsopPkt *mpkt_ptr = (RS32_MsopPkt *)pkt;
            if (mpkt_ptr->header.id != RS32_MSOP_ID)
            {
                //      rs_print(RS_ERROR, "[RS32] MSOP pkt ID no match.");
                return -2;
            }

            int first_azimuth;
            first_azimuth = RS_SWAP_SHORT(mpkt_ptr->blocks[0].azimuth);

            float temperature = this->computeTemperatue(mpkt_ptr->header.temp_raw);

            for (int blk_idx = 0; blk_idx < RS32_BLOCKS_PER_PKT; blk_idx++)
            {
                if (mpkt_ptr->blocks[blk_idx].id != RS32_BLOCK_ID)
                {
                    break;
                }
                int azimuth_blk = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx].azimuth);
                int azi_prev = 0;
                int azi_cur = 0;
                if (this->echo_mode_ == RS_ECHO_DUAL)
                {
                    if (blk_idx < (RS32_BLOCKS_PER_PKT - 2)) // 12
                    {
                        azi_prev = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx + 2].azimuth);
                        azi_cur = azimuth_blk;
                    }
                    else
                    {
                        azi_prev = azimuth_blk;
                        azi_cur = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx - 2].azimuth);
                    }
                }
                else
                {
                    if (blk_idx < (RS32_BLOCKS_PER_PKT - 1)) // 12
                    {
                        azi_prev = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx + 1].azimuth);
                        azi_cur = azimuth_blk;
                    }
                    else
                    {
                        azi_prev = azimuth_blk;
                        azi_cur = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx - 1].azimuth);
                    }
                }

                float azimuth_diff = (float)((36000 + azi_prev - azi_cur) % 36000);
                float azimuth_channel;
                for (int channel_idx = 0; channel_idx < RS32_CHANNELS_PER_BLOCK; channel_idx++)
                {
                    int azimuth_final;

                    azimuth_channel = azimuth_blk + (azimuth_diff * RS32_CHANNEL_TOFFSET * (channel_idx % 16) / RS32_FIRING_TDURATION);
                    azimuth_final = this->azimuthCalibration(azimuth_channel, channel_idx);

                    int idx_map = channel_idx;

                    float intensity = mpkt_ptr->blocks[blk_idx].channels[idx_map].intensity;
                    int distance = RS_SWAP_SHORT(mpkt_ptr->blocks[blk_idx].channels[idx_map].distance);
                    float distance_cali = distance * RS_RESOLUTION_5mm_DISTANCE_COEF;

                    int angle_horiz_ori;
                    int angle_horiz = (azimuth_final + 36000) % 36000;
                    int angle_vert;
                    angle_horiz_ori = (int)(azimuth_channel + 36000) % 36000;
                    angle_vert = (((int)(this->vert_angle_list_[channel_idx]) % 36000) + 36000) % 36000;

                    //store to pointcloud buffer
                    vpoint point;
                    if ((distance_cali <= this->max_distance_ && distance_cali >= this->min_distance_) && ((this->angle_flag_ && angle_horiz >= this->start_angle_ && angle_horiz <= this->end_angle_) || (!this->angle_flag_ && ((angle_horiz >= this->start_angle_ && angle_horiz <= 36000) || (angle_horiz >= 0 && angle_horiz <= this->end_angle_)))))
                    {
                        const double vert_cos_value = this->cos_lookup_table_[angle_vert];
                        const double horiz_cos_value = this->cos_lookup_table_[angle_horiz];
                        const double horiz_ori_cos_value = this->cos_lookup_table_[angle_horiz_ori];
                        point.x = distance_cali * vert_cos_value * horiz_cos_value + this->Rx_ * horiz_ori_cos_value;

                        const double horiz_sin_value = this->sin_lookup_table_[angle_horiz];
                        const double horiz_ori_sin_value = this->sin_lookup_table_[angle_horiz_ori];
                        point.y = -distance_cali * vert_cos_value * horiz_sin_value - this->Rx_ * horiz_ori_sin_value;

                        const double vert_sin_value = this->sin_lookup_table_[angle_vert];
                        point.z = distance_cali * vert_sin_value + this->Rz_;

                        point.intensity = intensity;
                        if (std::isnan(point.intensity))
                        {
                            point.intensity = 0;
                        }
                    }
                    else
                    {
                        point.x = NAN;
                        point.y = NAN;
                        point.z = NAN;
                        point.intensity = NAN;
                    }

#ifdef RS_POINT_COMPLEX
                    point.distance = distance_cali;
                    point.ring_id = channel_idx;
                    point.echo_id = (this->echo_mode_ == RS_ECHO_DUAL) ? (blk_idx % 2) : 0;
#endif
                    vec.push_back(point);
                }
            }

            return first_azimuth;
        }

        template <typename vpoint>
        int32_t Decoder32<vpoint>::decodeDifopPkt(const uint8_t *pkt)
        {

            RS32_DifopPkt *rs32_ptr = (RS32_DifopPkt *)pkt;
            if (rs32_ptr->id != RS32_DIFOP_ID)
            {
                //		rs_print(RS_ERROR, "[RS32] DIFOP pkt ID no match.");
                return -2;
            }

            if (rs32_ptr->return_mode == 0x01 || rs32_ptr->return_mode == 0x02)
            {
                this->echo_mode_ = rs32_ptr->return_mode;
            }
            else
            {
                this->echo_mode_ = RS_ECHO_DUAL;
            }

            int pkt_rate = ceil(RS32_POINTS_CHANNEL_PER_SECOND / RS32_BLOCKS_CHANNEL_PER_PKT);

            if (this->echo_mode_ == RS_ECHO_DUAL)
            {
                pkt_rate = pkt_rate * 2;
            }
            this->pkts_per_frame_ = ceil(pkt_rate * 60 / this->rpm_);

            if (!(this->cali_data_flag_ & 0x2))
            {
                bool angle_flag = true;
                const uint8_t *p_ver_cali;
                p_ver_cali = ((RS32_DifopPkt *)pkt)->pitch_cali;
                if ((p_ver_cali[0] == 0x00 || p_ver_cali[0] == 0xFF) &&
                    (p_ver_cali[1] == 0x00 || p_ver_cali[1] == 0xFF) &&
                    (p_ver_cali[2] == 0x00 || p_ver_cali[2] == 0xFF))
                {
                    angle_flag = false;
                }
                if (angle_flag)
                {
                    int lsb, mid, msb, neg = 1;
                    const uint8_t *p_hori_cali = ((RS32_DifopPkt *)pkt)->yaw_cali;
                    for (int i = 0; i < 32; i++)
                    {
                        /* vert angle calibration data */
                        lsb = p_ver_cali[i * 3];
                        mid = p_ver_cali[i * 3 + 1];
                        msb = p_ver_cali[i * 3 + 2];
                        if (lsb == 0)
                        {
                            neg = 1;
                        }
                        else if (lsb == 1)
                        {
                            neg = -1;
                        }
                        this->vert_angle_list_[i] = (mid * 256 + msb) * neg * 0.1f; // / 180 * M_PI;

                        /* horizon angle calibration data */
                        lsb = p_hori_cali[i * 3];
                        mid = p_hori_cali[i * 3 + 1];
                        msb = p_hori_cali[i * 3 + 2];
                        if (lsb == 0)
                        {
                            neg = 1;
                        }
                        else if (lsb == 1)
                        {
                            neg = -1;
                        }

                        this->hori_angle_list_[i] = (mid * 256 + msb) * neg * 0.1f;
                    }
                    this->cali_data_flag_ = this->cali_data_flag_ | 0x2;
                }
            }

            return 0;
        }

        template <typename vpoint>
        void Decoder32<vpoint>::loadCalibrationFile(const std::string &angle_path)
        {
            int row_index = 0;
            int laser_num = 32;
            std::string line_str;
            // read angle.csv
            std::ifstream fd_angle(angle_path.c_str(), std::ios::in);
            if (!fd_angle.is_open())
            {
                //        rs_print(RS_WARNING, "[RS32] Calibration file: %s does not exist!", angle_file_path.c_str());
                // std::cout << angle_file_path << " does not exist"<< std::endl;
            }
            else
            {
                row_index = 0;
                while (std::getline(fd_angle, line_str))
                {
                    std::stringstream ss(line_str);
                    std::string str;
                    std::vector<std::string> vect_str;
                    while (std::getline(ss, str, ','))
                    {
                        vect_str.push_back(str);
                    }
                    this->vert_angle_list_[row_index] = std::stof(vect_str[0]) * 100; // degree
                    this->hori_angle_list_[row_index] = std::stof(vect_str[1]) * 100; // degree
                    row_index++;
                    if (row_index >= laser_num)
                    {
                        break;
                    }
                }
                fd_angle.close();
            }
        }
    } // namespace lidar
} // namespace robosense