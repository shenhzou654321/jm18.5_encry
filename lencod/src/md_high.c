/*!
 ***************************************************************************
 * \file md_high.c
 *
 * \brief
 *    Main macroblock mode decision functions and helpers
 *
 **************************************************************************
 */

#include <math.h>
#include <limits.h>
#include <float.h>

#include "global.h"
#include "rdopt_coding_state.h"
#include "intrarefresh.h"
#include "image.h"
#include "ratectl.h"
#include "mode_decision.h"
#include "mode_decision_p8x8.h"
#include "fmo.h"
#include "me_umhex.h"
#include "me_umhexsmp.h"
#include "macroblock.h"
#include "md_common.h"
#include "conformance.h"
#include "vlc.h"
#include "rdopt.h"
#include "mv_search.h"

/*!
*************************************************************************************
* \brief
*    Mode Decision for a macroblock
*************************************************************************************
*/
void encode_one_macroblock_high (Macroblock *currMB)
{
  Slice *currSlice = currMB->p_Slice;
  VideoParameters *p_Vid = currMB->p_Vid;
  InputParameters *p_Inp = currMB->p_Inp;
  PicMotionParams **motion = p_Vid->enc_picture->mv_info;
  RDOPTStructure  *p_RDO = currSlice->p_RDO;

  int         max_index = 9;
  int         block, index, mode, i, j;
  RD_PARAMS   enc_mb;
  distblk     bmcost[5] = {DISTBLK_MAX};
  distblk     cost=0;
  distblk     min_cost = DISTBLK_MAX;
  int         intra1 = 0;
  int         mb_available[3];  //

  short       bslice      = (short) (currSlice->slice_type == B_SLICE);
  short       pslice      = (short) ((currSlice->slice_type == P_SLICE) || (currSlice->slice_type == SP_SLICE));
  short       intra       = (short) ((currSlice->slice_type == I_SLICE) || (currSlice->slice_type == SI_SLICE) || (pslice && currMB->mb_y == p_Vid->mb_y_upd && p_Vid->mb_y_upd != p_Vid->mb_y_intra));
  int         lambda_mf[3];

  imgpel    **mb_pred  = currSlice->mb_pred[0];
  Block8x8Info *b8x8info = p_Vid->b8x8info;

  char        chroma_pred_mode_range[2];
  short       inter_skip = 0;
  BestMode    md_best;
  Info8x8     best;


  init_md_best(&md_best);  //��ʼ�����ģʽ

  // Init best (need to create simple function)
  best.pdir = 0;
  best.bipred = 0;
  best.ref[LIST_0] = 0;
  best.ref[LIST_1] = -1;

  //���ǿ��ת��Ϊ֡��ģʽ(non-exec)
  intra |= RandomIntra (p_Vid, currMB->mbAddrX);    // Forced Pseudo-Random Intra

  //===== Setup Macroblock encoding parameters =====
  init_enc_mb_params(currMB, &enc_mb, intra);
  if (p_Inp->AdaptiveRounding)
  {
    reset_adaptive_rounding(p_Vid);
  }

  if (currSlice->mb_aff_frame_flag)
  {
    reset_mb_nz_coeff(p_Vid, currMB->mbAddrX);
  }

  //=====   S T O R E   C O D I N G   S T A T E   =====
  //---------------------------------------------------
  currSlice->store_coding_state (currMB, currSlice->p_RDO->cs_cm);

  if (!intra) //֡��
  {
    //===== set skip/direct motion vectors =====
    if (enc_mb.valid[0])
    {
      if (bslice)
        currSlice->Get_Direct_Motion_Vectors (currMB);
      else 
        FindSkipModeMotionVector (currMB);
    }
    if (p_Inp->CtxAdptLagrangeMult == 1)
    {
      get_initial_mb16x16_cost(currMB);
    }

    //===== MOTION ESTIMATION FOR 16x16, 16x8, 8x16 BLOCKS =====
    //  ����mode1��mode2��mode3
    for (mode = 1; mode < 4; mode++)
    {
      best.mode = (char) mode;
      best.bipred = 0;
      b8x8info->best[mode][0].bipred = 0;

      if (enc_mb.valid[mode])
      {
        for (cost=0, block=0; block<(mode==1?1:2); block++)
        {
          update_lambda_costs(currMB, &enc_mb, lambda_mf);
          //����ָ�,�������ָ�������˶���������С����
          PartitionMotionSearch (currMB, mode, block, lambda_mf);

          //--- set 4x4 block indices (for getting MV) ---
          j = (block==1 && mode==2 ? 2 : 0);
          i = (block==1 && mode==3 ? 2 : 0);

          //--- get cost and reference frame for List 0 prediction ---
          bmcost[LIST_0] = DISTBLK_MAX;
          list_prediction_cost(currMB, LIST_0, block, mode, &enc_mb, bmcost, best.ref);

          //����B_SLICE
          if (bslice)
          {
            //--- get cost and reference frame for List 1 prediction ---
            bmcost[LIST_1] = DISTBLK_MAX;
            list_prediction_cost(currMB, LIST_1, block, mode, &enc_mb, bmcost, best.ref);

            // Compute bipredictive cost between best list 0 and best list 1 references
            list_prediction_cost(currMB, BI_PRED, block, mode, &enc_mb, bmcost, best.ref);

            // currently Bi predictive ME is only supported for modes 1, 2, 3 and ref 0
            if (is_bipred_enabled(p_Vid, mode))
            {
              get_bipred_cost(currMB, mode, block, i, j, &best, &enc_mb, bmcost);
            }
            else
            {
              bmcost[BI_PRED_L0] = DISTBLK_MAX;
              bmcost[BI_PRED_L1] = DISTBLK_MAX;
            }

            // Determine prediction list based on mode cost
            determine_prediction_list(bmcost, &best, &cost);
          }
          else // if (bslice)
          {
            best.pdir = 0;
            cost      += bmcost[LIST_0];
          }

          assign_enc_picture_params(currMB, mode, &best, 2 * block);

          //----- set reference frame and direction parameters -----
          set_block8x8_info(b8x8info, mode, block, &best);

          //--- set reference frames and motion vectors ---
          if (mode>1 && block == 0)
            currSlice->set_ref_and_motion_vectors (currMB, motion, &best, block);
        } // for (block=0; block<(mode==1?1:2); block++)
        if (cost < min_cost)
        {
          md_best.mode = (byte) mode;
          md_best.cost = cost;
          currMB->best_mode = (short) mode;
          min_cost  = cost;
          if (p_Inp->CtxAdptLagrangeMult == 1)
          {
            adjust_mb16x16_cost(currMB, cost);
          }
        }
      } // if (enc_mb.valid[mode])
    } // for (mode=1; mode<4; mode++)

    //����P8x8���˶�����(�ֿ��Էֳ�:8x8 8x4 4x8 4x4)
    if (enc_mb.valid[P8x8])
    {    
      currMB->valid_8x8 = FALSE;

      //����8x8�����任
      if (p_Inp->Transform8x8Mode)
      {
        ResetRD8x8Data(p_Vid, p_RDO->tr8x8);
        currMB->luma_transform_size_8x8_flag = TRUE; //switch to 8x8 transform size
        //===========================================================
        // Check 8x8 partition with transform size 8x8
        //===========================================================
        //=====  LOOP OVER 8x8 SUB-PARTITIONS  (Motion Estimation & Mode Decision) =====
        //8x8 8x4 4x8 4x4 0~3
        for (block = 0; block < 4; block++)
        {
          //submacroblock_mode_decision����ָ�룬�������submacroblock_mode_decision_p_slice��
          //submacroblock_mode_decision_b_slice���������ڽ�һ������÷ָ�
          currSlice->submacroblock_mode_decision(currMB, &enc_mb, p_RDO->tr8x8, p_RDO->cofAC8x8ts[block], block, &cost);
          if(!currMB->valid_8x8)
            break;
          set_subblock8x8_info(b8x8info, P8x8, block, p_RDO->tr8x8);
        } 

      }// if (p_Inp->Transform8x8Mode)

      currMB->valid_4x4 = FALSE;
      //����8x8�����ֳ�8x8 8x4 4x8 4x4
      if (p_Inp->Transform8x8Mode != 2)
      {
        currMB->luma_transform_size_8x8_flag = FALSE; //switch to 8x8 transform size
        ResetRD8x8Data(p_Vid, p_RDO->tr4x4);
        //=================================================================
        // Check 8x8, 8x4, 4x8 and 4x4 partitions with transform size 4x4
        //=================================================================
        //=====  LOOP OVER 8x8 SUB-PARTITIONS  (Motion Estimation & Mode Decision) =====
        for (block = 0; block < 4; block++)
        {
          currSlice->submacroblock_mode_decision(currMB, &enc_mb, p_RDO->tr4x4, p_RDO->coefAC8x8[block], block, &cost);
          if(!currMB->valid_4x4)
            break;
          set_subblock8x8_info(b8x8info, P8x8, block, p_RDO->tr4x4);
        }
      }// if (p_Inp->Transform8x8Mode != 2)

      if (p_Inp->RCEnable)
        rc_store_diff(currSlice->diffy, &p_Vid->pCurImg[currMB->opix_y], currMB->pix_x, mb_pred);

      p_Vid->giRDOpt_B8OnlyFlag = FALSE;
    }
  }
  else // if (!intra)
  {
    min_cost = DISTBLK_MAX;
  }
  
  //ɫ�ȵ����֡��Ԥ��ģʽ��ѡ��ֻ��Ҫ�Ƚ�4��ģʽ�Ĵ���,ѡ����С���ۼ���
  //�Ƚ���ɫ��֡��Ԥ��,Ȼ����ѡ�����ģʽ
  set_chroma_pred_mode(currMB, enc_mb, mb_available, chroma_pred_mode_range);

  //��min_rdcost��ѡ��һ����ѵĺ��ģʽ
  //========= C H O O S E   B E S T   M A C R O B L O C K   M O D E =========
  //-------------------------------------------------------------------------
    printf("\033[1;33m tmp_test============chroma_pred_mode_range[0]=%d [1]=%d ======= \033[0m \n",
    chroma_pred_mode_range[0],chroma_pred_mode_range[1]);
  for (currMB->c_ipred_mode = chroma_pred_mode_range[0]; currMB->c_ipred_mode<=chroma_pred_mode_range[1]; currMB->c_ipred_mode++)
  {
    // bypass if c_ipred_mode is not allowed
    /*
       1:DCԤ��,DCԤ��������4X4��Ϊ������λ,��A,B,C�������ȡƽ��ֵ,����û��A,B,C���ʱ��,ȡ128��
	2:��ֱԤ��,����B������,ֱ�Ӱ�B������±ߵ�����Copy
	3:ˮƽԤ��,����A������,ֱ�Ӱ�A������ұߵ�����Copy
	4:ƽ̹ģʽ,��ҪA,B,Cͬʱ����,ƽ̹ģʽ��Ԥ������ֵ�㷨�Ƚϸ���
       */
    if ( (p_Vid->yuv_format != YUV400) &&
      (  ((!intra || !p_Inp->IntraDisableInterOnly) && p_Inp->ChromaIntraDisable == 1 && currMB->c_ipred_mode!=DC_PRED_8) 
      || (currMB->c_ipred_mode == VERT_PRED_8 && !mb_available[0])  //A�Ϻ�鲻����
      || (currMB->c_ipred_mode == HOR_PRED_8  && !mb_available[1])  //B���鲻����
      || (currMB->c_ipred_mode == PLANE_8     && (!mb_available[1] || !mb_available[0] || !mb_available[2]))))
      continue;        

    //===== GET BEST MACROBLOCK MODE =====
    //0~9 
    /*
        *mb_mode_table[10]  = 
        *{
        *       0,      PSKIP/BSKIP_DIRECT
        *       1,      P16x16
        *       2,      P16x8
        *       3,      P8x16
        *       8,      P8x8 
        *       10     I16MB,
        *       9       I4MB,
        *       13     I8MB,
        *       14     IPCM, 
        *       12     SI4MB
        *};
        */
    //ѭ��9��
    for (index=0; index < max_index; index++)
    {
      mode = mb_mode_table[index];  //�Ӻ��ģʽ���еõ���Ӧ��ģʽֵ
      //printf("mode %d %7.3f", mode, (double) currMB->min_rdcost);
      if (enc_mb.valid[mode])
      {
        //printf(" mode %d is valid", mode);
        if (p_Vid->yuv_format != YUV400)
        {           
          currMB->i16mode = 0; 
        }

        // Skip intra modes in inter slices if best mode is inter <P8x8 with cbp equal to 0   
        //������֡�����������ģʽ��֡��ģʽ��CBP=0��֡��ģʽ
        if (currSlice->P444_joined)
        {
          if (p_Inp->SkipIntraInInterSlices && !intra && mode >= I16MB 
            && currMB->best_mode <=3 && currMB->best_cbp == 0 && currSlice->cmp_cbp[1] == 0 && currSlice->cmp_cbp[2] == 0 && (currMB->min_rdcost < weighted_cost(enc_mb.lambda_mdfp,5)))
            continue;
        }
        else
        {
          if (p_Inp->SkipIntraInInterSlices) //non-exec
          {
            if (!intra && mode >= I4MB)
            {
              if (currMB->best_mode <=3 && currMB->best_cbp == 0 && (currMB->min_rdcost < weighted_cost(enc_mb.lambda_mdfp, 5)))
              {
                continue;
              }
              else if (currMB->best_mode == 0 && (currMB->min_rdcost < weighted_cost(enc_mb.lambda_mdfp,6)))
              {
                continue;
              }
            }
          }
        }

        //compute the min_rdcost of all mode ={skip,I16x16 I16x8 I8x16 P8x8 I16MB I4MB I8MB IPCM}
        //��������ģʽ����С����ֵѭ��9��
        compute_mode_RD_cost(currMB, &enc_mb, (short) mode, &inter_skip);
        
      }
      //printf(" best %d %7.2f\n", currMB->best_mode, (double) currMB->min_rdcost);
    }// for (index=0; index<max_index; index++)
  }// for (currMB->c_ipred_mode=DC_PRED_8; currMB->c_ipred_mode<=chroma_pred_mode_range[1]; currMB->c_ipred_mode++)                     

  restore_nz_coeff(currMB);

  intra1 = is_intra(currMB);

  //=====  S E T   F I N A L   M A C R O B L O C K   P A R A M E T E R S ======
  //---------------------------------------------------------------------------
  update_qp_cbp_tmp(currMB, p_RDO->cbp);
  currSlice->set_stored_mb_parameters (currMB);

  // Rate control
  if(p_Inp->RCEnable && p_Inp->RCUpdateMode <= MAX_RC_MODE)
    rc_store_mad(currMB);


  //===== Decide if this MB will restrict the reference frames =====
  if (p_Inp->RestrictRef)
    update_refresh_map(currMB, intra, intra1);
}


