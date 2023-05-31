#include <zephyr.h>
#include <errno.h>
#include <cortex_m/tz.h>
#include <power/reboot.h>
#include <sys/util.h>
#include <autoconf.h>
#include <string.h>
#include <bl_validation.h>

#include "nrf_cc3xx_platform.h"
#include "nrf_cc3xx_platform_kmu.h"
#include "mbedtls/cc3xx_kmu.h"
#include "mbedtls/aes.h"
#include "mbedtls/ctr_drbg.h"
#include <mbedtls/ecdsa.h>
#include <drivers/entropy.h>

#define   AES_KMU_SLOT			  2

#define ECPARAMS    MBEDTLS_ECP_DP_SECP256K1

#if !defined(ECPARAMS)
#define ECPARAMS    mbedtls_ecp_curve_list()->grp_id
#endif

#define	   TEST_VERFY_SIGNATURE	

#define      PRIV_STR_BUF_SIZE        133
#define   PRIV_STR_LEN           (PRIV_STR_BUF_SIZE-1)
#define   PUB_STR_BUF_SIZE        197
#define  KEY_HEX_SIZE        184
#define  PRIV_HEX_SIZE        66
#define  PUB_HEX_SIZE        (KEY_HEX_SIZE - PRIV_HEX_SIZE)
#define  PUB_HEX_ADDR(a)    (a+PRIV_HEX_SIZE)
#define  PUB_STR_ADDR(a)    (a+PRIV_STR_LEN)
#define  COM_PUB_STR_ADD(a)  (a+PRIV_STR_LEN+130)
#define  UNCOM_PUB_STR_ADD(a) (a+PRIV_STR_LEN)

//#define IOTEX_USE_PSA_API

#define TV_NAME(name) name " -- [" __FILE__ ":" STRINGIFY(__LINE__) "]"

typedef struct {
	const uint32_t src_line_num; /**< Test vector source file line number. */
	const uint32_t curve_type; /**< Curve type for test vector. */
	const int expected_sign_err_code; /**< Expected error code from ECDSA sign
									   operation. */
	const int expected_verify_err_code; /**< Expected result of following ECDSA
										 verify operation. */
	const char *p_test_vector_name; /**< Pointer to ECDSA test vector name. */
	char * p_input; /**< Pointer to ECDSA hash input in hex string format. */
	const char *
		p_qx; /**< Pointer to ECDSA public key X component in hex string
					   format. */
	const char *
		p_qy; /**< Pointer to ECDSA public key Y component in hex string
					   format. */
	const char *
		p_x; /**< Pointer to ECDSA private key component in hex string format. */
} test_vector_ecdsa_sign_t;


test_vector_ecdsa_sign_t test_case_ecdsa_data = {
	.curve_type = ECPARAMS,
	.expected_sign_err_code = 0,
	.expected_verify_err_code = 0,
	.p_test_vector_name = TV_NAME("secp256r1 valid SHA256 1")
};

static test_vector_ecdsa_sign_t *p_test_vector_sign;
static char* m_ecdsa_input_buf;
static size_t hash_len, initOKFlg=0;
static int entropy_func(void *ctx, unsigned char *buf, size_t len)
{
	return entropy_get_entropy(ctx, buf, len);
}


#if defined(MBEDTLS_CTR_DRBG_C)
mbedtls_ctr_drbg_context ctr_drbg_ctx;
int (*drbg_random)(void *, unsigned char *, size_t) = &mbedtls_ctr_drbg_random;

static int init_drbg(const unsigned char *p_optional_seed, size_t len)
{
	static const unsigned char ncs_seed[] = "ncs_drbg_seed";

	const unsigned char *p_seed;

	if (p_optional_seed == NULL) {
		p_seed = ncs_seed;
		len = sizeof(ncs_seed);
	} else {
		p_seed = p_optional_seed;
	}

	const struct device *p_device = device_get_binding(DT_LABEL(DT_CHOSEN(zephyr_entropy)));

	if (p_device == NULL)
		return -ENODEV;

	// Ensure previously run test is properly deallocated
	// (This frees the mutex inside ctr_drbg context)
	mbedtls_ctr_drbg_free(&ctr_drbg_ctx);
	mbedtls_ctr_drbg_init(&ctr_drbg_ctx);
	return mbedtls_ctr_drbg_seed(&ctr_drbg_ctx, entropy_func, (void *)p_device,p_seed, len);
}
#elif defined(MBEDTLS_HMAC_DRBG_C)
mbedtls_hmac_drbg_context drbg_ctx;
static int (*drbg_random)(void *, unsigned char *, size_t) = &mbedtls_hmac_drbg_random;

int init_drbg(const unsigned char *p_optional_seed, size_t len)
{
	static const unsigned char ncs_seed[] = "ncs_drbg_seed";

	const unsigned char *p_seed;

	if (p_optional_seed == NULL) {
		p_seed = ncs_seed;
		len = sizeof(ncs_seed);
	} else {
		p_seed = p_optional_seed;
	}

	// Ensure previously run test is properly deallocated
	// (This frees the mutex inside hmac_drbg context)
	mbedtls_hmac_drbg_free(&drbg_ctx);
	mbedtls_hmac_drbg_init(&drbg_ctx);

	const struct device *p_device = device_get_binding(DT_LABEL(DT_CHOSEN(zephyr_entropy)));

	if (!p_device)
		return -ENODEV;

	const mbedtls_md_info_t *p_info =
		mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

	return mbedtls_hmac_drbg_seed(&drbg_ctx, p_info, entropy_func,
		(void *)p_device, p_seed, len);
}
#endif
#if defined(MBEDTLS_CTR_DRBG_C) || defined(MBEDTLS_HMAC_DRBG_C)
#ifdef IOTEX_USE_PSA_API
static uint8_t pub_buf[65];
#ifdef	TEST_VERFY_SIGNATURE
static void exec_test_case_ecdsa_sign(char *buf,int *len, int *ret_code)
#else
static void exec_test_case_ecdsa_sign(char *buf,int *len )
#endif
{
	int err_code = -1;
	unsigned int len_r,len_s;
	
	/* Prepare signer context. */
	mbedtls_ecdsa_context ctx_sign;
	mbedtls_ecdsa_init(&ctx_sign);

	err_code = mbedtls_ecp_group_load(&ctx_sign.grp,p_test_vector_sign->curve_type);

#ifdef TEST_VERFY_SIGNATURE
	/* Get public key. */
	err_code = mbedtls_ecp_point_read_string(&ctx_sign.Q, 16,p_test_vector_sign->p_qx,p_test_vector_sign->p_qy);
	
#endif	
	/* Get private key. */
	err_code = mbedtls_mpi_read_string(&ctx_sign.d, 16,p_test_vector_sign->p_x);

	/* Verify keys. */
#ifdef TEST_VERFY_SIGNATURE	
	err_code = mbedtls_ecp_check_pubkey(&ctx_sign.grp, &ctx_sign.Q);	
#endif

	err_code = mbedtls_ecp_check_privkey(&ctx_sign.grp, &ctx_sign.d);
	/* Prepare and generate the ECDSA signature. */
	/* Note: The contexts do not contain these (as is the case for e.g. Q), so simply share them here. */
	mbedtls_mpi r;
	mbedtls_mpi s;
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);

	//start_time_measurement();

	err_code = mbedtls_ecdsa_sign(&ctx_sign.grp, &r, &s, &ctx_sign.d,m_ecdsa_input_buf, hash_len,drbg_random, &ctr_drbg_ctx);    
	//stop_time_measurement();
    len_r = mbedtls_mpi_size(&r);
    len_s  = mbedtls_mpi_size(&s);
    mbedtls_mpi_write_binary(&r, buf, len_r);
    mbedtls_mpi_write_binary(&s, buf+len_r, len_s);
	*len = len_r+len_s;

#ifdef TEST_VERFY_SIGNATURE
	//  verify  signature
	mbedtls_ecdsa_context ctx_verify;
	mbedtls_ecdsa_init(&ctx_verify);
	/* Transfer public EC information. */
	err_code = mbedtls_ecp_group_copy(&ctx_verify.grp, &ctx_sign.grp);	
	/* Transfer public key. */
	err_code = mbedtls_ecp_copy(&ctx_verify.Q, &ctx_sign.Q);
	/* Verify the generated ECDSA signature by running the ECDSA verify. */
	err_code = mbedtls_ecdsa_verify(&ctx_verify.grp, m_ecdsa_input_buf,
					hash_len, &ctx_verify.Q, &r, &s);
	*ret_code = err_code;		
#endif

	/* Free resources. */
	mbedtls_mpi_free(&r);
	mbedtls_mpi_free(&s);
	mbedtls_ecdsa_free(&ctx_sign);
#ifdef	TEST_VERFY_SIGNATURE
	mbedtls_ecdsa_free(&ctx_verify);
#endif
}
#endif

__TZ_NONSECURE_ENTRY_FUNC
int initECDSA_sep256r(void)
{	
	if(Initcc3xx())
		return -1;		
    if (init_drbg(NULL, 0) != 0) {
		initOKFlg = 0;        	
		return  1;
    }   		
	initOKFlg = 1;
	return 0;
}

#ifdef IOTEX_USE_PSA_API
__TZ_NONSECURE_ENTRY_FUNC
int doESDA_sep256r_Sign(char *inbuf, uint32_t len, char *buf, int* sinlen)
{
	static char shaBuf[32];
	int err_code = -1, i;
	int ret_code = 0;
	if(!initOKFlg)
		return -1;	

	p_test_vector_sign = &test_case_ecdsa_data;

	err_code = mbedtls_sha256_ret(inbuf, len,  shaBuf,false);
	
	p_test_vector_sign->p_input = shaBuf;
	m_ecdsa_input_buf = shaBuf;
	hash_len = 32;

#ifdef	TEST_VERFY_SIGNATURE
	exec_test_case_ecdsa_sign(buf, sinlen, &ret_code);
	return ret_code;
#else
	exec_test_case_ecdsa_sign(buf, sinlen);
	return err_code;
#endif
}
#endif

void hex2str(char* buf_hex, int len, char *str)
{
	int i,j;
    const char hexmap[] = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
    };	
	for(i =0,j=0; i< len; i++)
	{		
		str[j++] = hexmap[buf_hex[i]>>4];	
		str[j++] = hexmap[buf_hex[i]&0x0F];
	}
	str[j] = 0;	
}
static char str2Hex(char c)
{
    if (c >= '0' && c <= '9') {
        return (c - '0');
    }

    if (c >= 'a' && c <= 'z') {
        return (c - 'a' + 10);
    }

    if (c >= 'A' && c <= 'Z') {
        return (c -'A' + 10);
    }
    return c;
}

int hexStr2Bin(char *str, char *bin) {
    int i,j;
    for(i = 0,j = 0; j < (strlen(str)>>1) ; i++,j++)
    {
        bin[j] = (str2Hex(str[i]) <<4);
        i++;
        bin[j] |= str2Hex(str[i]);
    }
    return j; 
}

__TZ_NONSECURE_ENTRY_FUNC
int GenRandom(char *out)
{	
	char buf[8];
	drbg_random(&ctr_drbg_ctx, buf, sizeof(buf));
	hex2str(buf, sizeof(buf),out);
	return  0;
}

__TZ_NONSECURE_ENTRY_FUNC
int iotex_get_random(char *output, unsigned int output_size)
{	
	drbg_random(&ctr_drbg_ctx, output, output_size);

	return  0;
}

__TZ_NONSECURE_ENTRY_FUNC
int iotex_random(void)
{
	union
	{
		int  dat;
		char buf[4];
	}random32;
	
	drbg_random(&ctr_drbg_ctx, random32.buf, sizeof(random32.buf));
	return random32.dat;
}


#ifdef IOTEX_USE_PSA_API
__TZ_NONSECURE_ENTRY_FUNC
int gen_ecc_key(char *buf, int  *len, char* buf_p, int *len_p)
{	
	int ret = 0, strLen;
	mbedtls_ecdsa_context ctx_sign;
	mbedtls_ecdsa_init( &ctx_sign );	

    if(( ret = mbedtls_ecdsa_genkey(&ctx_sign, ECPARAMS,drbg_random, &ctr_drbg_ctx)) != 0)
    {        	  		
		return ret;
    }	
	
	// get private key in the format of  hex-string
	if((ret = mbedtls_mpi_write_string(&ctx_sign.d, 16, buf, *len, len) != 0))
	{		
		return ret;		
	}
	if((ret = mbedtls_ecp_point_write_binary( &ctx_sign.grp, &ctx_sign.Q, MBEDTLS_ECP_PF_UNCOMPRESSED, len_p, buf_p, *len_p )) != 0)
	{		
		return ret;		
	}
    strLen = 60;
	if((ret = mbedtls_ecp_point_write_binary( &ctx_sign.grp, &ctx_sign.Q, MBEDTLS_ECP_PF_COMPRESSED, &strLen, buf_p + *len_p, strLen )) != 0)
	{		
		return ret;		
	}
	*len_p = *len_p|(strLen<<16);	
	return ret;
}
#endif

uint8_t px_str[65] = {0};
uint8_t px_bin[32] = {0};

uint8_t pqx_str[65] = {0};
uint8_t pqy_str[65] = {0};
uint8_t pq_bin[65] = {0};

#ifdef TEST_VERFY_SIGNATURE
void SetEccPrivKey(uint8_t *key, uint8_t *key_p)
#else
void SetEccPrivKey(uint8_t *key)
#endif
{
    static uint8_t decrypted_buf[66];
#ifdef TEST_VERFY_SIGNATURE
	static uint8_t decrypted_buf_px[66];
	static uint8_t decrypted_buf_py[66];
	memcpy(decrypted_buf_px, key_p, 64);	

	decrypted_buf_px[64]=0;
	test_case_ecdsa_data.p_qx = (const char *)decrypted_buf_px;
	memcpy(decrypted_buf_py, key_p+64, 64);
	decrypted_buf_py[64]=0;
	test_case_ecdsa_data.p_qy = (const char *)decrypted_buf_py;

	memcpy(pqx_str, decrypted_buf_px, 65);
	memcpy(pqy_str, decrypted_buf_py, 65);

	pq_bin[0] = 0x04;
	hexStr2Bin(decrypted_buf_px, (char *)pq_bin + 1);
	hexStr2Bin(decrypted_buf_py, (char *)pq_bin + 33);
#endif	

    memcpy(decrypted_buf, key, 64);
    decrypted_buf[64] = 0;
    test_case_ecdsa_data.p_x = (const char *)decrypted_buf;
	memcpy(px_str, decrypted_buf, 65);
	hexStr2Bin(decrypted_buf, px_bin);
}
__TZ_NONSECURE_ENTRY_FUNC
void genAESKey(uint8_t *key, int len)
{
    drbg_random(&ctr_drbg_ctx, key, sizeof(key));
}

__TZ_NONSECURE_ENTRY_FUNC
int  updateKey(uint8_t *key)
{
    uint8_t decrypted_buf[PRIV_STR_BUF_SIZE];
    uint8_t binBuf[PRIV_STR_BUF_SIZE];
    unsigned char pub[PUB_STR_BUF_SIZE];

	memcpy(decrypted_buf, key, PRIV_STR_LEN);
	decrypted_buf[PRIV_STR_LEN] = 0;
	hexStr2Bin(decrypted_buf, binBuf);
	for (int i = 0; i < 4; i++) {
		if (cc3xx_decrypt(AES_KMU_SLOT, decrypted_buf+(i<<4), binBuf+(i<<4))) {
			return -1;
		}
	}
	decrypted_buf[64] = 0;
#ifdef TEST_VERFY_SIGNATURE    
	memcpy(pub, UNCOM_PUB_STR_ADD(key)+2, 128);
	pub[128] = 0;
	SetEccPrivKey(decrypted_buf, pub);
#else
	memcpy(pub, UNCOM_PUB_STR_ADD(key), 130);
	pub[130] = 0;
	SetEccPrivKey(decrypted_buf);
#endif
	return 0;
}

__TZ_NONSECURE_ENTRY_FUNC
int getPrikeyWithStr(unsigned char *pxbuf)
{
	if ((pxbuf) && (px_str[0]))
	{
		memcpy(pxbuf, px_str, 65);
		return 0;
	}

	return -1;		
}

__TZ_NONSECURE_ENTRY_FUNC
int getPubkeyWithStr(unsigned char *pqxbuf, unsigned char *pqybuf)
{
	int ret = 0;
	if ((pqxbuf) && (pqx_str[0]))
	{
		memcpy(pqxbuf, pqx_str, 65);
		ret |= 0x1;
	}

	if ((pqybuf) && (pqy_str[0]))
	{
		memcpy(pqybuf, pqy_str, 65);
		ret |= 0x2;
	}

	return ret;		
}

__TZ_NONSECURE_ENTRY_FUNC
int getKeyWithBin(unsigned char *pxbuf, unsigned char *pqbuf)
{
	int ret = 0;
	if ((pxbuf) && (px_bin[0]))
	{
		memcpy(pxbuf, px_bin, 32);
		ret |= 0x1;
	}
	
	if ((pqbuf) && (pq_bin[0]))
	{
		memcpy(pqbuf, pq_bin, 65);
		ret |= 0x2;
	}

	return ret;
}

#endif
