
/*
 * This file is part of the OpenKinect Project. http://www.openkinect.org
 *
 * Copyright (c) 2014 individual OpenKinect contributors. See the CONTRIB file
 * for details.
 *
 * This code is licensed to you under the terms of the Apache License, version
 * 2.0, or, at your option, the terms of the GNU General Public License,
 * version 2.0. See the APACHE20 and GPL2 files for the text of the licenses,
 * or the following URLs:
 * http://www.apache.org/licenses/LICENSE-2.0
 * http://www.gnu.org/licenses/gpl-2.0.txt
 *
 * If you redistribute this file in source form, modified or unmodified, you
 * may:
 *   1) Leave this header intact and distribute it under the same terms,
 *      accompanying it with the APACHE20 and GPL20 files, or
 *   2) Delete the Apache 2.0 clause and accompany it with the GPL2 file, or
 *   3) Delete the GPL v2 clause and accompany it with the APACHE20 file
 * In all cases you must keep the copyright notice intact and include a copy
 * of the CONTRIB file.
 *
 * Binary distributions must follow the binary distribution requirements of
 * either License.
 */

/** @file CpuKde_depth_packet_processor.cpp Depth processor implementation for the CpuKde. */


#include <fstream>
#include <iostream>
#include <algorithm>
#include "processors.h"
#include "libfreenect2_data_structures.h"
#include <stdio.h>
#include <cstdlib>
#include <string>
#include <cstring>

#define _USE_MATH_DEFINES
#include <math.h>

#include <cmath>
#include <limits>

static float k_list[30] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
static float n_list[30] = {0.0f, 0.0f, 1.0f, 1.0f, 2.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f, 4.0f, 4.0f, 3.0f, 4.0f, 4.0f, 5.0f, 5.0f, 5.0f, 6.0f, 5.0f, 6.0f, 6.0f, 7.0f, 7.0f, 8.0f, 8.0f, 7.0f, 8.0f, 9.0f, 9.0f};
static float m_list[30] = {0.0f, 1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f, 4.0f, 4.0f, 5.0f, 5.0f, 6.0f, 6.0f, 7.0f, 7.0f, 7.0f, 7.0f, 8.0f, 8.0f, 9.0f, 9.0f, 10.0f, 10.0f, 11.0f, 11.0f, 12.0f, 12.0f, 13.0f, 13.0f, 14.0f};


class CpuKdeDepthPacketProcessorImpl
{
public:
  Mat<uint16_t> p0_table0, p0_table1, p0_table2;
  Mat<float> x_table, z_table;

  int16_t lut11to16[2048];

  float trig_table0[512*424][6];
  float trig_table1[512*424][6];
  float trig_table2[512*424][6];

  bool enable_bilateral_filter, enable_edge_filter;
  Parameters params;

  float *conf_frame, *depth_frame;
  float *gauss_filt_kernel;
  bool flip_ptables;

  CpuKdeDepthPacketProcessorImpl()
  {

  }

  void init(Parameters in_params)
  {
    params = in_params;
    enable_bilateral_filter = in_params.enable_bilateral_filter;
    enable_edge_filter = in_params.enable_edge_filter;
    createGaussianKernel(&gauss_filt_kernel, params.kde_neigborhood_size);
    flip_ptables = true;
  }
  /** Allocate a new IR frame. */
  void newConfFrame()
  {
    conf_frame = new float[512*424];
    
    //ir_frame = new Frame(512, 424, 12);
  }

  ~CpuKdeDepthPacketProcessorImpl()
  {
    //delete ir_frame;
    //delete depth_frame;
  }

  /** Allocate a new depth frame. */
  void newDepthFrame()
  {
    depth_frame = new float[512*424];
  }

  void createGaussianKernel(float** kernel, int size)
  {
	*kernel = new float[2*size+1];
	float sigma = 0.5f*(float)size;

	for(int i = -size; i <= size; i++)	
	{
		(*kernel)[i+size] = std::exp(-0.5f*i*i/(sigma*sigma)); 	
	}
  }

  int32_t decodePixelMeasurement(unsigned char* data, int sub, int x, int y)
  {
    if (x < 1 || y < 0 || 510 < x || 423 < y)
    {
      return lut11to16[0];
    }

    int r1zi = (x >> 2) + ((x & 0x3) << 7); // Range 1..510
    r1zi = r1zi * 11L; // Range 11..5610

    // 298496 = 512 * 424 * 11 / 8 = number of bytes per sub image
    uint16_t *ptr = reinterpret_cast<uint16_t *>(data + 298496 * sub);
    int i = y < 212 ? y + 212 : 423 - y;
    ptr += 352*i;

    int r1yi = r1zi >> 4; // Range 0..350
    r1zi = r1zi & 15;

    int i1 = ptr[r1yi];
    int i2 = ptr[r1yi + 1];
    i1 = i1 >> r1zi;
    i2 = i2 << (16 - r1zi);

    return lut11to16[((i1 | i2) & 2047)];
  }

  /**
   * Initialize cos and sin trigonometry tables for each of the three #phase_in_rad parameters.
   * @param p0table Angle at every (x, y) position.
   * @param [out] trig_tables (3 cos tables, followed by 3 sin tables for the three phases.
   */
  void fillTrigTable(Mat<uint16_t> &p0table, float trig_table[512*424][6])
  {
    int i = 0;

    for(int y = 0; y < 424; ++y)
      for(int x = 0; x < 512; ++x, ++i)
      {
        float p0 = -((float)p0table.at(y, x)) * 0.000031 * M_PI;

        float tmp0 = p0 + params.phase_in_rad[0];
        float tmp1 = p0 + params.phase_in_rad[1];
        float tmp2 = p0 + params.phase_in_rad[2];

        trig_table[i][0] = std::cos(tmp0);
        trig_table[i][1] = std::cos(tmp1);
        trig_table[i][2] = std::cos(tmp2);

        trig_table[i][3] = std::sin(-tmp0);
        trig_table[i][4] = std::sin(-tmp1);
        trig_table[i][5] = std::sin(-tmp2);
      }
  }

  /**
   * Process measurement (all three layers).
   * @param [in] trig_table Trigonometry tables.
   * @param abMultiplierPerFrq Multiplier.
   * @param x X position in the image.
   * @param y Y position in the image.
   * @param m Measurement.
   * @param [out] m_out Processed measurement (IR a, IR b, IR amplitude).
   */
  void processMeasurementTriple(float trig_table[512*424][6], float abMultiplierPerFrq, int x, int y, const int32_t* m, float* m_out)
  {
    float zmultiplier = z_table.at(y, x);
    if (0 < zmultiplier)
    {
      bool saturated = (m[0] == 32767 || m[1] == 32767 || m[2] == 32767);
      if (!saturated)
      {
        int offset = y * 512 + x;
        float cos_tmp0 = trig_table[offset][0];
        float cos_tmp1 = trig_table[offset][1];
        float cos_tmp2 = trig_table[offset][2];

        float sin_negtmp0 = trig_table[offset][3];
        float sin_negtmp1 = trig_table[offset][4];
        float sin_negtmp2 = trig_table[offset][5];

        // formula given in Patent US 8,587,771 B2
        float ir_image_a = cos_tmp0 * m[0] + cos_tmp1 * m[1] + cos_tmp2 * m[2];
        float ir_image_b = sin_negtmp0 * m[0] + sin_negtmp1 * m[1] + sin_negtmp2 * m[2];

        // only if modeMask & 32 != 0;
        if(true)//(modeMask & 32) != 0)
        {
            ir_image_a *= abMultiplierPerFrq;
            ir_image_b *= abMultiplierPerFrq;
        }
        float ir_amplitude = std::sqrt(ir_image_a * ir_image_a + ir_image_b * ir_image_b) * params.ab_multiplier;

        m_out[0] = ir_image_a;
        m_out[1] = ir_image_b;
        m_out[2] = ir_amplitude;
      }
      else
      {
        // Saturated pixel.
        m_out[0] = 0;
        m_out[1] = 0;
        m_out[2] = 65535.0;
      }
    }
    else
    {
      // Invalid pixel.
      m_out[0] = 0;
      m_out[1] = 0;
      m_out[2] = 0;
    }
  }

  /**
   * Transform measurement.
   * @param [in, out] m Measurement.
   */
  void transformMeasurements(float* m, float* m_out)
  {
    float tmp0 = std::atan2((m[1]), (m[0]));
    tmp0 = tmp0 < 0 ? tmp0 + M_PI * 2.0f : tmp0;
    tmp0 = (tmp0 != tmp0) ? 0 : tmp0;

    float tmp1 = std::sqrt(m[0] * m[0] + m[1] * m[1]) * params.ab_multiplier;

    m_out[0] = tmp0; // phase
    m_out[1] = tmp1; // ir amplitude - (possibly bilateral filtered)
  }


  /**
   * Process first pixel stage.
   * @param x Horizontal position.
   * @param y Vertical position.
   * @param data
   * @param [out] m0_out First layer output.
   * @param [out] m1_out Second layer output.
   * @param [out] m2_out Third layer output.
   */
  void processPixelStage1(int x, int y, unsigned char* data, float *m0_out, float *m1_out, float *m2_out)
  {
    int32_t m0_raw[3], m1_raw[3], m2_raw[3];

    m0_raw[0] = decodePixelMeasurement(data, 0, x, y);
    m0_raw[1] = decodePixelMeasurement(data, 1, x, y);
    m0_raw[2] = decodePixelMeasurement(data, 2, x, y);
    m1_raw[0] = decodePixelMeasurement(data, 3, x, y);
    m1_raw[1] = decodePixelMeasurement(data, 4, x, y);
    m1_raw[2] = decodePixelMeasurement(data, 5, x, y);
    m2_raw[0] = decodePixelMeasurement(data, 6, x, y);
    m2_raw[1] = decodePixelMeasurement(data, 7, x, y);
    m2_raw[2] = decodePixelMeasurement(data, 8, x, y);

    processMeasurementTriple(trig_table0, params.ab_multiplier_per_frq[0], x, y, m0_raw, m0_out);
    processMeasurementTriple(trig_table1, params.ab_multiplier_per_frq[1], x, y, m1_raw, m1_out);
    processMeasurementTriple(trig_table2, params.ab_multiplier_per_frq[2], x, y, m2_raw, m2_out);
  }

  /**
   * Filter pixels in stage 1.
   * @param x Horizontal position.
   * @param y Vertical position.
   * @param m Input data?
   * @param [out] Output data.
   * @param [out] bilateral_max_edge_test Whether the accumulated distance of each image stayed within limits.
   */
  void filterPixelStage1(int x, int y, const Mat<Vec<float, 9> >& m, float* m_out, bool& bilateral_max_edge_test)
  {
    const float *m_ptr = (m.ptr(y, x)->val);
    bilateral_max_edge_test = true;

    if(x < 1 || y < 1 || x > 510 || y > 422)
    {
      for(int i = 0; i < 9; ++i)
        m_out[i] = m_ptr[i];
    }
    else
    {
      float m_normalized[2];
      float other_m_normalized[2];

      int offset = 0;

      for(int i = 0; i < 3; ++i, m_ptr += 3, m_out += 3, offset += 3)
      {
        float norm2 = m_ptr[0] * m_ptr[0] + m_ptr[1] * m_ptr[1];
        float inv_norm = 1.0f / std::sqrt(norm2);
        inv_norm = (inv_norm == inv_norm) ? inv_norm : std::numeric_limits<float>::infinity();

        m_normalized[0] = m_ptr[0] * inv_norm;
        m_normalized[1] = m_ptr[1] * inv_norm;

        int j = 0;

        float weight_acc = 0.0f;
        float weighted_m_acc[2] = {0.0f, 0.0f};

        float threshold = (params.joint_bilateral_ab_threshold * params.joint_bilateral_ab_threshold) / (params.ab_multiplier * params.ab_multiplier);
        float joint_bilateral_exp = params.joint_bilateral_exp;

        if(norm2 < threshold)
        {
          threshold = 0.0f;
          joint_bilateral_exp = 0.0f;
        }

        float dist_acc = 0.0f;

        for(int yi = -1; yi < 2; ++yi)
        {
          for(int xi = -1; xi < 2; ++xi, ++j)
          {
            if(yi == 0 && xi == 0)
            {
              weight_acc += params.gaussian_kernel[j];

              weighted_m_acc[0] += params.gaussian_kernel[j] * m_ptr[0];
              weighted_m_acc[1] += params.gaussian_kernel[j] * m_ptr[1];
              continue;
            }

            const float *other_m_ptr = (m.ptr(y + yi, x + xi)->val) + offset;
            float other_norm2 = other_m_ptr[0] * other_m_ptr[0] + other_m_ptr[1] * other_m_ptr[1];
            // TODO: maybe fix numeric problems when norm = 0 - original code uses reciprocal square root, which returns +inf for +0
            float other_inv_norm = 1.0f / std::sqrt(other_norm2);
            other_inv_norm = (other_inv_norm == other_inv_norm) ? other_inv_norm : std::numeric_limits<float>::infinity();

            other_m_normalized[0] = other_m_ptr[0] * other_inv_norm;
            other_m_normalized[1] = other_m_ptr[1] * other_inv_norm;

            float dist = -(other_m_normalized[0] * m_normalized[0] + other_m_normalized[1] * m_normalized[1]);
            dist += 1.0f;
            dist *= 0.5f;

            float weight = 0.0f;

            if(other_norm2 >= threshold)
            {
              weight = (params.gaussian_kernel[j] * std::exp(-1.442695f * joint_bilateral_exp * dist));
              dist_acc += dist;
            }

            weighted_m_acc[0] += weight * other_m_ptr[0];
            weighted_m_acc[1] += weight * other_m_ptr[1];

            weight_acc += weight;
          }
        }

        bilateral_max_edge_test = bilateral_max_edge_test && dist_acc < params.joint_bilateral_max_edge;

        m_out[0] = 0.0f < weight_acc ? weighted_m_acc[0] / weight_acc : 0.0f;
        m_out[1] = 0.0f < weight_acc ? weighted_m_acc[1] / weight_acc : 0.0f;
        m_out[2] = m_ptr[2];
      }
    }
  }

    void calcErr(const float k, const float n, const float m, const float t0, const float t1, const float t2, float* err1, float* err2, float* err3)
    {
            //phase unwrapping equation residuals
        *err1 = 3.0f*n-15.0f*k-(t1-t0);
        *err2 = 3.0f*n-2.0f*m-(t2-t0);
        *err3 = 15.0f*k-2.0f*m-(t2-t1);
    }

    /********************************************************************************
     * Rank all 30 phase hypothses and returns the two most likley
     ********************************************************************************/
    void phaseUnWrapper(float t0, float t1,float t2, float* phase_first, float* phase_second, float* err_w1, float* err_w2)
    {
      float err;
      float err1,err2,err3;

        //unwrapping weight for cost function
        float w1 = 1.0f;
        float w2 = 10.0f;
        float w3 = 1.0218f;

        float err_min=100000.0f;
        float err_min_second = 200000.0f;
        unsigned int ind_min, ind_second;

        float k,n,m;

        for(int i=0; i<30; i++)
        {
            m = m_list[i];
            n = n_list[i];
            k = k_list[i];
            calcErr(k,n,m,t0,t1,t2,&err1,&err2,&err3);
            err = w1*err1*err1+w2*err2*err2+w3*err3*err3;
            if(err<err_min)
            {
                err_min_second = err_min;
                ind_second = ind_min;
                err_min = err;
                ind_min = i;

            }
            else if(err<err_min_second)
            {
            err_min_second = err;
                ind_second = i;
            }

        }

        //decode ind_min
        float mvals = m_list[ind_min];
        float nvals = n_list[ind_min];
        float kvals = k_list[ind_min];

        float phi2_out = (t2/2.0f+mvals);
        float phi1_out = (t1/15.0f+kvals);
        float phi0_out = (t0/3.0f+nvals);

        *err_w1 = err_min;

        //phase fusion
        *phase_first = (phi2_out+phi1_out+phi0_out)/3.0f;

        mvals = m_list[ind_second];
        nvals = n_list[ind_second];
        kvals = k_list[ind_second];

        phi2_out = (t2/2.0f+mvals);
        phi1_out = (t1/15.0f+kvals);
        phi0_out = (t0/3.0f+nvals);

        *err_w2 = err_min_second;

        //phase fusion
        *phase_second = (phi2_out+phi1_out+phi0_out)/3.0f;

    }

    void phaseUnWrapper3(float t0, float t1,float t2, float* phase_first, float* phase_second, float* phase_third, float* err_w1, float* err_w2, float* err_w3)
    {
      float err;
      float err1,err2,err3;

	    float w1 = 1.0f;
	    float w2 = 10.0f;
	    float w3 = 1.0218f;
	
	    float err_min=100000.0f;
	    float err_min_second = 200000.0f;
	    float err_min_third = 300000.0f;
	    unsigned int ind_min, ind_second, ind_third;

	    float k,n,m;
	
	    for(int i=0; i<30; i++)
	    {
		    m = m_list[i];
		    n = n_list[i];
		    k = k_list[i];
		    calcErr(k,n,m,t0,t1,t2,&err1,&err2,&err3);
		    err = w1*err1*err1+w2*err2*err2+w3*err3*err3;
		    if(err<err_min)
		    {
			    err_min_third = err_min_second;
			    ind_third = ind_second;
			    err_min_second = err_min;
			    ind_second = ind_min;
			    err_min = err;
			    ind_min = i;

		    }
		    else if(err<err_min_second)
		    {
			    err_min_third = err_min_second;
			    ind_third = ind_second;
        	err_min_second = err;
			    ind_second = i;
		    }
		    else if(err<err_min_third)
		    {
        	err_min_third = err;
			    ind_third = i;
		    }
		
	    }

	    //decode ind_min
	    float mvals = m_list[ind_min];
	    float nvals = n_list[ind_min];
	    float kvals = k_list[ind_min];

	    float phi2_out = (t2/2.0f+mvals);
	    float phi1_out = (t1/15.0f+kvals);
	    float phi0_out = (t0/3.0f+nvals);

	    *err_w1 = err_min;

	    *phase_first = (phi2_out+phi1_out+phi0_out)/3.0f;

	    mvals = m_list[ind_second];
	    nvals = n_list[ind_second];
	    kvals = k_list[ind_second];

	    phi2_out = (t2/2.0f+mvals);
	    phi1_out = (t1/15.0f+kvals);
	    phi0_out = (t0/3.0f+nvals);	

	    *err_w2 = err_min_second;
	    *phase_second = (phi2_out+phi1_out+phi0_out)/3.0f;

	    mvals = m_list[ind_third];
	    nvals = n_list[ind_third];
	    kvals = k_list[ind_third];

	    phi2_out = (t2/2.0f+mvals);
	    phi1_out = (t1/15.0f+kvals);
	    phi0_out = (t0/3.0f+nvals);

	    *err_w3 = err_min_third;
	    *phase_third = (phi2_out+phi1_out+phi0_out)/3.0f;
    }

    /*******************************************************************************
     * Predict phase variance from amplitude direct quadratic model
     ******************************************************************************/
    void calculatePhaseUnwrappingVarDirect(float ir0, float ir1, float ir2, float* var0, float* var1, float* var2)
    {
        //Model: sigma = 1/(gamma0*a+gamma1*a^2+gamma2). The gammas are optimized using lsqnonlin in matlab.
        //For more details see the paper "Efficient Phase Unwrapping using Kernel Density Estimation"
        //section 3.3 and 4.4.
        float alpha_max = 0.5f*M_PI;

        float q0 = ir0 > 5.244404f ? 0.7919451669f*ir0-0.002363097609f*ir0*ir0-3.088285897f : 1.0f/alpha_max;
        float q1 = ir1 > 4.084835 ? 1.214266794f*ir1-0.00581082634f*ir1*ir1-3.863119924f : 1.0f/alpha_max;
        float q2 = ir2 > 6.379475 ? 0.6101457464f*ir2-0.00113679233f*ir2*ir2-2.84614442f : 1.0f/alpha_max;

      	float alpha0 = 1.0f/q0;
        alpha0 = alpha0 > alpha_max? alpha_max : alpha0;
        float alpha1 = 1.0f/q1;
        alpha1 = alpha1 > alpha_max ? alpha_max : alpha1;
        float alpha2 = 1.0f/q2;
        alpha2 = alpha2 > alpha_max ? alpha_max : alpha2;

        alpha0 = alpha0 < 0.001f ? 0.001f: alpha0;
        alpha1 = alpha1 < 0.001f ? 0.001f: alpha1;
        alpha2 = alpha2 < 0.001f ? 0.001f: alpha2;

        *var0 = alpha0*alpha0;
        *var1 = alpha1*alpha1;
        *var2 = alpha2*alpha2;
    }


    /*******************************************************************************
     * Predict phase variance from amplitude quadratic atan model
     ******************************************************************************/
		void calculatePhaseUnwrappingVar(float ir0, float ir1, float ir2, float* var0, float* var1, float* var2)
    {
       //Model: sigma = atan(sqrt(1/(gamma0*a+gamma1*a^2+gamma2)-1)). The gammas are optimized using lsqnonlin in matlab. 
			//For more details see the paper "Efficient Phase Unwrapping using Kernel Density Estimation",
			//section 3.3 and 4.4.
			float q0 = 0.8211288451f*ir0-0.002601348899f*ir0*ir0-3.549793908f;
			float q1 = 1.259642407f*ir1-0.005478390508f*ir1*ir1-4.335841127f;
			float q2 = 0.6447928035f*ir2-0.0009627273649f*ir2*ir2-3.368205575f;
			q0*=q0;
			q1*=q1;
			q2*=q2;

			float sigma0 = q0>1.0f ? atan(sqrt(1.0f/(q0-1.0f))) : ir0 > 5.64173671f ? 5.64173671f*0.5f*M_PI/ir0 : 0.5f*M_PI;
			float sigma1 = q1>1.0f ? atan(sqrt(1.0f/(q1-1.0f))) : ir1 > 4.31705182f ? 4.31705182f*0.5f*M_PI/ir1 : 0.5f*M_PI;
			float sigma2 = q2>1.0f ? atan(sqrt(1.0f/(q2-1.0f))) : ir2 > 6.84453530f ? 6.84453530f*0.5f*M_PI/ir2 : 0.5f*M_PI;

			//Set sigma = 0.001 to the minimum standard deviation of the phase
			sigma0 = sigma0 < 0.001f ? 0.001f: sigma0;
			sigma1 = sigma1 < 0.001f ? 0.001f: sigma1;
			sigma2 = sigma2 < 0.001f ? 0.001f: sigma2;

			*var0 = sigma0*sigma0;
			*var1 = sigma1*sigma1;
			*var2 = sigma2*sigma2;

    }

  void processPixelStage2_phase(int x, int y, float *m0_in, float *m1_in, float *m2_in, float *phase0, float *phase1, float* conf0, float* conf1)
  {
    float m0[2], m1[2], m2[2];
    transformMeasurements(m0_in, m0);
    transformMeasurements(m1_in, m1);
    transformMeasurements(m2_in, m2);
    m0[1] = std::isnan(m0[1]) ? 0.0f : m0[1];
    m1[1] = std::isnan(m1[1]) ? 0.0f : m1[1];
    m2[1] = std::isnan(m2[1]) ? 0.0f : m2[1];
    float ir_sum = m0[1] + m1[1] + m2[1];

    float phase_likelihood;
    // if(DISABLE_DISAMBIGUATION)
    float phase_first = 0.0;
	float phase_second = 0.0;

	float J_1, J_2, unwrapping_likelihood1, unwrapping_likelihood2;

	//scale with least common multiples of modulation frequencies
	float t0 = m0[0] / (2.0f * M_PI) * 3.0f;
	float t1 = m1[0] / (2.0f * M_PI) * 15.0f;;
	float t2 = m2[0] / (2.0f * M_PI) * 2.0f;;

	//rank and extract two most likely phase hypothises
	phaseUnWrapper(t0, t1, t2, &phase_first, &phase_second, &J_1, &J_2);
	if(ir_sum < 0.4f*65535.0f)
	{
		//calculate phase likelihood from amplitude
		float var0,var1,var2;
		if(strcmp(params.phase_noise_prediction_method.c_str(),"arctan quadratic") == 0)
        calculatePhaseUnwrappingVar(m0[1], m1[1], m2[1], &var0, &var1, &var2);
    else if(strcmp(params.phase_noise_prediction_method.c_str(),"direct quadratic") == 0)
        calculatePhaseUnwrappingVarDirect(m0[1], m1[1], m2[1], &var0, &var1, &var2);
    else
        calculatePhaseUnwrappingVar(m0[1], m1[1], m2[1], &var0, &var1, &var2); //use default
    
		phase_likelihood = exp(-(var0+var1+var2)/(2.0f*params.phase_confidence_scale));
		phase_likelihood = isnan(phase_likelihood) ? 0.0f: phase_likelihood;
	}
	else
	{
		phase_likelihood = 0.0f;
	}

	//merge phase likelihood with phase likelihood
	unwrapping_likelihood1 = phase_likelihood*exp(-J_1/(2*params.unwrapping_likelihood_scale));
	unwrapping_likelihood2 = phase_likelihood*exp(-J_2/(2*params.unwrapping_likelihood_scale));

	//suppress confidence if phase is beyond allowed range
	unwrapping_likelihood1 = phase_first > params.max_depth*9.0f/18750.0f ? 0.0f: unwrapping_likelihood1;
	unwrapping_likelihood2 = phase_second > params.max_depth*9.0f/18750.0f ? 0.0f: unwrapping_likelihood2;

    int ind = x+512*y;
    phase0[ind] = phase_first;
    phase1[ind] = phase_second;
    conf0[ind] = unwrapping_likelihood1;
    conf1[ind] = unwrapping_likelihood2;
  }

  void processPixelStage2_phase3(int x, int y, float *m0_in, float *m1_in, float *m2_in, float *phase0, float *phase1, float *phase2, float* conf0, float* conf1, float* conf2)
  {
    float m0[2], m1[2], m2[2];
    transformMeasurements(m0_in, m0);
    transformMeasurements(m1_in, m1);
    transformMeasurements(m2_in, m2);
    m0[1] = std::isnan(m0[1]) ? 0.0f : m0[1];
    m1[1] = std::isnan(m1[1]) ? 0.0f : m1[1];
    m2[1] = std::isnan(m2[1]) ? 0.0f : m2[1];
    float ir_sum = m0[1] + m1[1] + m2[1];

    float phase_first = 0.0;
	float phase_second = 0.0;
	float phase_third = 0.0;
	float J_1, J_2, J_3, unwrapping_likelihood1, unwrapping_likelihood2, unwrapping_likelihood3;

	//scale with least common multiples of modulation frequencies
	float t0 = m0[0] / (2.0f * M_PI) * 3.0f;
	float t1 = m1[0] / (2.0f * M_PI) * 15.0f;;
	float t2 = m2[0] / (2.0f * M_PI) * 2.0f;;

	//rank and extract three most likely phase hypothises
    phaseUnWrapper3(t0, t1, t2, &phase_first, &phase_second, &phase_third, &J_1, &J_2, &J_3);
    float phase_likelihood;
	if(ir_sum < 0.4f*65535.0f)
	{
		//calculate phase likelihood from amplitude
		float var0,var1,var2;
		calculatePhaseUnwrappingVar(m0[1], m1[1], m2[1], &var0, &var1, &var2);
		phase_likelihood = exp(-(var0+var1+var2)/(2.0f*params.phase_confidence_scale));
		phase_likelihood = isnan(phase_likelihood) ? 0.0f: phase_likelihood;
	}
	else
	{
		phase_likelihood = 0.0f;
	}

  unwrapping_likelihood1 = phase_likelihood*exp(-J_1/(2*params.unwrapping_likelihood_scale));
	unwrapping_likelihood2 = phase_likelihood*exp(-J_2/(2*params.unwrapping_likelihood_scale));
	unwrapping_likelihood3 = phase_likelihood*exp(-J_3/(2*params.unwrapping_likelihood_scale));

	//suppress confidence if phase is beyond allowed range
	unwrapping_likelihood1 = phase_first > params.max_depth*9.0f/18750.0f ? 0.0f: unwrapping_likelihood1;
	unwrapping_likelihood2 = phase_second > params.max_depth*9.0f/18750.0f ? 0.0f: unwrapping_likelihood2;
	unwrapping_likelihood3 = phase_third > params.max_depth*9.0f/18750.0f ? 0.0f: unwrapping_likelihood3;

    int ind = x+512*y;
    phase0[ind] = phase_first;
    phase1[ind] = phase_second;
    phase2[ind] = phase_third;
    conf0[ind] = unwrapping_likelihood1;
    conf1[ind] = unwrapping_likelihood2;
    conf2[ind] = unwrapping_likelihood3;
  }

  void filter_kde(const unsigned int i, float* phase1, float* phase2, float* conf1, float* conf2, const float* gauss_filt_array, float* depth_out, float* max_val_arr)
  {
	float kde_val_1, kde_val_2;

	const int unsigned loadX = i % 512;
	const int unsigned loadY = i / 512;

	int k, l;
    float sum_1, sum_2;

	//initialize neighborhood boundaries
	int from_x = (loadX > params.kde_neigborhood_size ? -params.kde_neigborhood_size : -loadX+1);
	int from_y = (loadY > params.kde_neigborhood_size ? -params.kde_neigborhood_size : -loadY+1);
	int to_x = (loadX < 511-params.kde_neigborhood_size-1 ? params.kde_neigborhood_size: 511-loadX-1);
	int to_y = (loadY < 423-params.kde_neigborhood_size  ? params.kde_neigborhood_size: 423-loadY);

    kde_val_1 = 0.0f;
	kde_val_2 = 0.0f;
	float phase_local1 = phase1[i];
	float phase_local2 = phase2[i];

	if(loadX >= 1 && loadX < 511 && loadY<424)
    {
        sum_1=0.0f;
		sum_2=0.0f;
		float gauss;
		float sum_gauss = 0.0f;

		float phase_1_local;
		float phase_2_local;
		float conf1_local;
		float conf2_local;
        //float4 phase_conf_local;
		unsigned int ind;

		float diff11, diff21, diff12, diff22;

		//calculate KDE for all hypothesis within the neigborhood
		for(k=from_y; k<=to_y; k++)
		  for(l=from_x; l<=to_x; l++)
	    {
				ind = (loadY+k)*512+(loadX+l);

				conf1_local = conf1[ind];
				conf2_local = conf2[ind];

				phase_1_local = phase1[ind];
				phase_2_local = phase2[ind];

				gauss = gauss_filt_array[k+params.kde_neigborhood_size]*gauss_filt_array[l+params.kde_neigborhood_size];
				sum_gauss += gauss*(conf1_local+conf2_local);
				diff11 = phase_1_local-phase_local1;
				diff21 = phase_2_local-phase_local1;
				diff12 = phase_1_local-phase_local2;
				diff22 = phase_2_local-phase_local2;
                sum_1 += gauss*(conf1_local*std::exp(-diff11*diff11/(2*params.kde_sigma_sqr))+conf2_local*std::exp(-diff21*diff21/(2*params.kde_sigma_sqr)));
				sum_2 += gauss*(conf1_local*std::exp(-diff12*diff12/(2*params.kde_sigma_sqr))+conf2_local*std::exp(-diff22*diff22/(2*params.kde_sigma_sqr)));
	    }
		kde_val_1 = sum_gauss > 0.5f ? sum_1/sum_gauss : sum_1*2.0f;
		kde_val_2 = sum_gauss > 0.5f ? sum_2/sum_gauss : sum_2*2.0f;
  }

	//select hypothesis
	int val_ind = kde_val_2 <= kde_val_1 ? 1: 0;

	float phase_final = val_ind ? phase_local1: phase_local2;
	float max_val = val_ind ? kde_val_1: kde_val_2;
	unsigned int x = i % 512;
	unsigned int y = 423 - (i / 512);
    float zmultiplier = z_table.at(y, x);
    float xmultiplier = x_table.at(y, x);

    phase_final = 0 < phase_final ? phase_final + params.phase_offset : phase_final;

    float depth_linear = zmultiplier * phase_final;
    float max_depth = phase_final * params.unambigious_dist * 2;

    bool cond1 = /*(modeMask & 32) != 0*/ true && 0 < depth_linear && 0 < max_depth;

    xmultiplier = (xmultiplier * 90) / (max_depth * max_depth * 8192.0);

    float depth_fit = depth_linear / (-depth_linear * xmultiplier + 1);

    depth_fit = depth_fit < 0 ? 0 : depth_fit;
    float depth = cond1 ? depth_fit : depth_linear; // r1.y -> later r2.z

    unsigned int out_i = x+512*y;
    // depth
    //std::cout<<out_i<<std::endl;
    depth_out[out_i] = depth;
    max_val_arr[out_i] = max_val;
  }

  void filter_kde3(const unsigned int i, float* phase1, float* phase2, float* phase3, float* conf1, float* conf2, float* conf3, const float* gauss_filt_array, float* depth_out, float* max_val_arr)
  {
	float kde_val_1, kde_val_2, kde_val_3;

	const int unsigned loadX = i % 512;
	const int unsigned loadY = i / 512;

	int k, l;
    float sum_1, sum_2, sum_3;

	//initialize neighborhood boundaries
	int from_x = (loadX > params.kde_neigborhood_size ? -params.kde_neigborhood_size : -loadX+1);
	int from_y = (loadY > params.kde_neigborhood_size ? -params.kde_neigborhood_size : -loadY+1);
	int to_x = (loadX < 511-params.kde_neigborhood_size-1 ? params.kde_neigborhood_size: 511-loadX-1);
	int to_y = (loadY < 423-params.kde_neigborhood_size  ? params.kde_neigborhood_size: 423-loadY);

    kde_val_1 = 0.0f;
	kde_val_2 = 0.0f;
    kde_val_3 = 0.0f;
	float phase_local1 = phase1[i];
	float phase_local2 = phase2[i];
    float phase_local3 = phase3[i];

	if(loadX >= 1 && loadX < 511 && loadY<424)
    {
        sum_1=0.0f;
		sum_2=0.0f;
		sum_3=0.0f;
		float gauss;
		float sum_gauss = 0.0f;
		unsigned int ind;

		float phase_1_local;
		float phase_2_local;
		float phase_3_local;
		float conf1_local;
		float conf2_local;
		float conf3_local;
        float diff11, diff12, diff13, diff21, diff22, diff23, diff31, diff32, diff33;

		//calculate KDE for all hypothesis within the neigborhood
		for(k=from_y; k<=to_y; k++)
		  for(l=from_x; l<=to_x; l++)
	        {
				ind = (loadY+k)*512+(loadX+l);

				conf1_local = conf1[ind];
				conf2_local = conf2[ind];
				conf3_local = conf3[ind];
				phase_1_local = phase1[ind];
				phase_2_local = phase2[ind];
				phase_3_local = phase3[ind];
				diff11 = phase_1_local-phase_local1;
				diff12 = phase_1_local-phase_local2;
				diff13 = phase_1_local-phase_local3;
				diff21 = phase_2_local-phase_local1;
				diff22 = phase_2_local-phase_local2;
				diff23 = phase_2_local-phase_local3;
				diff31 = phase_3_local-phase_local1;
				diff32 = phase_3_local-phase_local2;
				diff33 = phase_3_local-phase_local3;
				gauss = gauss_filt_array[k+params.kde_neigborhood_size]*gauss_filt_array[l+params.kde_neigborhood_size];
				sum_gauss += gauss*(conf1_local+conf2_local+conf3_local);
		        sum_1 += gauss*(conf1_local*exp(-diff11*diff11/(2*params.kde_sigma_sqr))+conf2_local*exp(-diff21*diff21/(2*params.kde_sigma_sqr))+conf3_local*exp(-diff31*diff31/(2*params.kde_sigma_sqr)));
				sum_2 += gauss*(conf1_local*exp(-diff12*diff12/(2*params.kde_sigma_sqr))+conf2_local*exp(-diff22*diff22/(2*params.kde_sigma_sqr))+conf3_local*exp(-diff32*diff32/(2*params.kde_sigma_sqr)));
				sum_3 += gauss*(conf1_local*exp(-diff13*diff13/(2*params.kde_sigma_sqr))+conf2_local*exp(-diff23*diff23/(2*params.kde_sigma_sqr))+conf3_local*exp(-diff33*diff33/(2*params.kde_sigma_sqr)));
	        }

		kde_val_1 = sum_gauss > 0.5f ? sum_1/sum_gauss : sum_1*2.0f;
		kde_val_2 = sum_gauss > 0.5f ? sum_2/sum_gauss : sum_2*2.0f;
        kde_val_3 = sum_gauss > 0.5f ? sum_3/sum_gauss : sum_3*2.0f;
    }

	//select hypothesis
    float phase_final, max_val;
	if(kde_val_2 > kde_val_1 || kde_val_3 > kde_val_1)
	{
		if(kde_val_3 > kde_val_2)
		{
			phase_final = phase_local3;
			max_val = kde_val_3;
		}
		else
		{
			phase_final = phase_local2;
			max_val = kde_val_2;
		}
	}
	else
	{
		phase_final = phase_local1;
		max_val = kde_val_1;
    }

	unsigned int x = i % 512;
	unsigned int y = 423 - (i / 512);
    float zmultiplier = z_table.at(y, x);
    float xmultiplier = x_table.at(y, x);

    phase_final = 0 < phase_final ? phase_final + params.phase_offset : phase_final;

    float depth_linear = zmultiplier * phase_final;
    float max_depth = phase_final * params.unambigious_dist * 2;

    bool cond1 = /*(modeMask & 32) != 0*/ true && 0 < depth_linear && 0 < max_depth;

    xmultiplier = (xmultiplier * 90) / (max_depth * max_depth * 8192.0);

    float depth_fit = depth_linear / (-depth_linear * xmultiplier + 1);

    depth_fit = depth_fit < 0 ? 0 : depth_fit;
    float depth = cond1 ? depth_fit : depth_linear; // r1.y -> later r2.z

    unsigned int out_i = x+512*y;
    // depth
    //std::cout<<out_i<<std::endl;
    depth_out[out_i] = depth;
    max_val_arr[out_i] = max_val;
  }
};

CpuKdeDepthPacketProcessor::CpuKdeDepthPacketProcessor() :
    impl_(new CpuKdeDepthPacketProcessorImpl())
{
}

CpuKdeDepthPacketProcessor::~CpuKdeDepthPacketProcessor()
{
  delete impl_;
}

/*void CpuKdeDepthPacketProcessor::setConfiguration(const libfreenect2::DepthPacketProcessor::Config &config)
{
  DepthPacketProcessor::setConfiguration(config);

  impl_->enable_bilateral_filter = config.EnableBilateralFilter;
  impl_->enable_edge_filter = config.EnableEdgeAwareFilter;
}*/

/**
 * Load p0 tables from a command response,
 * @param buffer Buffer containing the response.
 * @param buffer_length Length of the response data.
 */
void CpuKdeDepthPacketProcessor::loadP0TablesFromCommandResponse(unsigned char* buffer, size_t buffer_length)
{
  // TODO: check known header fields (headersize, tablesize)
  P0TablesResponse* p0table = (P0TablesResponse*)buffer;

  if(buffer_length < sizeof(P0TablesResponse))
  {
    std::cout << "P0Table response too short!\n";
    return;
  }

  if(impl_->flip_ptables)
  {
    flipHorizontal(Mat<uint16_t>(424, 512, p0table->p0table0), impl_->p0_table0);
    flipHorizontal(Mat<uint16_t>(424, 512, p0table->p0table1), impl_->p0_table1);
    flipHorizontal(Mat<uint16_t>(424, 512, p0table->p0table2), impl_->p0_table2);
  }
  else
  {
    Mat<uint16_t> p00(424, 512, p0table->p0table0);
    p00.copyTo(impl_->p0_table0);
    Mat<uint16_t>(424, 512, p0table->p0table1).copyTo(impl_->p0_table1);
    Mat<uint16_t>(424, 512, p0table->p0table2).copyTo(impl_->p0_table2);
  }

  impl_->fillTrigTable(impl_->p0_table0, impl_->trig_table0);
  impl_->fillTrigTable(impl_->p0_table1, impl_->trig_table1);
  impl_->fillTrigTable(impl_->p0_table2, impl_->trig_table2);
}

void CpuKdeDepthPacketProcessor::loadXZTables(const float *xtable, const float *ztable)
{
  impl_->x_table.create(424, 512);
  std::copy(xtable, xtable + TABLE_SIZE, impl_->x_table.ptr(0,0));

  impl_->z_table.create(424, 512);
  std::copy(ztable, ztable + TABLE_SIZE, impl_->z_table.ptr(0,0));
}

void CpuKdeDepthPacketProcessor::loadLookupTable(const short *lut)
{
  std::copy(lut, lut + LUT_SIZE, impl_->lut11to16);
}


void CpuKdeDepthPacketProcessor::initParameters(Parameters param)
{
    impl_->init(param);
}
/**
 * Process a packet.
 * @param packet Packet to process.
 */
void CpuKdeDepthPacketProcessor::process(unsigned char* buffer, float** depth_buffer, float** kde_conf)
{
  impl_->newConfFrame();
  impl_->newDepthFrame();

  Mat<Vec<float, 9> >
      m(424, 512),
      m_filtered(424, 512)
  ;
  Mat<unsigned char> m_max_edge_test(424, 512);

  float *m_ptr = (m.ptr(0, 0)->val);

  for(int y = 0; y < 424; ++y)
    for(int x = 0; x < 512; ++x, m_ptr += 9)
    {
      impl_->processPixelStage1(x, y, buffer, m_ptr + 0, m_ptr + 3, m_ptr + 6);
    }

  // bilateral filtering
  if(impl_->enable_bilateral_filter)
  {
    float *m_filtered_ptr = (m_filtered.ptr(0, 0)->val);
    unsigned char *m_max_edge_test_ptr = m_max_edge_test.ptr(0, 0);

    for(int y = 0; y < 424; ++y)
      for(int x = 0; x < 512; ++x, m_filtered_ptr += 9, ++m_max_edge_test_ptr)
      {
        bool max_edge_test_val = true;
        impl_->filterPixelStage1(x, y, m, m_filtered_ptr, max_edge_test_val);
        //*m_max_edge_test_ptr = max_edge_test_val ? 1 : 0;
      }

    m_ptr = (m_filtered.ptr(0, 0)->val);
  }
  else
  {
    m_ptr = (m.ptr(0, 0)->val);
  }
  float* phase = new float[512*424*impl_->params.num_hyps];
  float* conf = new float[512*424*impl_->params.num_hyps];

  for(int y = 0; y < 424; ++y)
      for(int x = 0; x < 512; ++x, m_ptr += 9)
      {
         switch(impl_->params.num_hyps)
         {
            case 2:
  	            impl_->processPixelStage2_phase(x, y, m_ptr + 0, m_ptr + 3, m_ptr + 6, phase + 0, phase+512*424, conf + 0, conf + 512*424);
                break;
            case 3:
                impl_->processPixelStage2_phase3(x, y, m_ptr + 0, m_ptr + 3, m_ptr + 6, phase + 0, phase+512*424, phase+512*424*2, conf + 0, conf + 512*424, conf + 512*424*2);
                break;
            default:
                std::cout<<"kde processor with "<<impl_->params.num_hyps<<" hypotheses not implemented\n";
         }
      }

  for(unsigned int i = 0; i < 512*424; ++i)
  {
     switch(impl_->params.num_hyps)
     {
        case 2:
            impl_->filter_kde(i, phase + 0, phase + 512*424, conf + 0, conf + 512*424, impl_->gauss_filt_kernel, impl_->depth_frame, impl_->conf_frame);
            break;
        case 3:
            impl_->filter_kde3(i, phase + 0, phase + 512*424, phase + 512*424*2, conf + 0, conf + 512*424, conf + 512*424*2, impl_->gauss_filt_kernel, impl_->depth_frame, impl_->conf_frame);
            break;
        default:
            std::cout<<"kde processor with "<<impl_->params.num_hyps<<" hypotheses not implemented\n";
     }
  }
  //impl_->stopTiming(LOG_INFO);

    *depth_buffer = impl_->depth_frame;
    *kde_conf = impl_->conf_frame;
    delete[] phase;
    delete[] conf;
}
