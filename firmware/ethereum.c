/*
 * This file is part of the TREZOR project.
 *
 * Copyright (C) 2016 Alex Beregszaszi <alex@rtfs.hu>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ethereum.h"
#include "fsm.h"
#include "layout2.h"
#include "messages.h"
#include "transaction.h"
#include "ecdsa.h"
#include "protect.h"
#include "crypto.h"
#include "secp256k1.h"
#include "sha3.h"
#include "util.h"

static bool signing = false;
static size_t data_left;
static EthereumTxRequest resp;
static uint8_t hash[32], sig[64], privkey[32];
// FIXME: this is currently 400 bytes. Could be probably improved.
struct SHA3_CTX keccak_ctx;

/*
 * Encode length according to RLP.
 * FIXME: improve
 */
static int rlp_encode_length(uint8_t *buf, int length, uint8_t firstbyte, bool list)
{
	if (!list && (length == 1 && firstbyte <= 0x7f)) {
		/* Extra-special case: null is encoded differently */
		buf[0] = !firstbyte ? 0x80 : firstbyte;
		return 1;
	} else if (length <= 55) {
		buf[0] = (list ? 0xc0 : 0x80) + length;
		return 1;
	} else if (length <= 0xff) {
		buf[0] = (list ? 0xf7 : 0xb7) + 1;
		buf[1] = length;
		return 2;
	} else if (length <= 0xffff) {
		buf[0] = (list ? 0xf7 : 0xb7) + 2;
		buf[1] = length >> 8;
		buf[2] = length & 0xff;
		return 3;
	} else {
		buf[0] = (list ? 0xf7 : 0xb7) + 3;
		buf[1] = length >> 16;
		buf[2] = length >> 8;
		buf[3] = length & 0xff;
		return 4;
	}
}

/*
 * Calculate the number of bytes needed for an RLP length header.
 * NOTE: supports up to 16MB of data (how unlikely...)
 * FIXME: improve
 */
static int rlp_calculate_length(int length, uint8_t firstbyte) {
	if (length == 1 && firstbyte <= 0x7f) {
		return 1;
	} else if (length <= 55) {
		return 1 + length;
	} else if (length <= 0xff) {
		return 2 + length;
	} else if (length <= 0xffff) {
		return 3 + length;
	} else {
		return 4 + length;
	}
}

static inline void hash_data(const uint8_t *buf, size_t size)
{
	sha3_Update(&keccak_ctx, buf, size);
}

/*
 * Push an RLP encoded length to the hash buffer.
 */
static void hash_rlp_length(int length, uint8_t firstbyte)
{
	uint8_t buf[4];
	size_t size = rlp_encode_length(buf, length, firstbyte, false);
	hash_data(buf, size);
}

/*
 * Push an RLP encoded list length to the hash buffer.
 */
static void hash_rlp_list_length(int length)
{
	uint8_t buf[4];
	size_t size = rlp_encode_length(buf, length, 0, true);
	hash_data(buf, size);
}

/*
 * Push an RLP encoded length field and data to the hash buffer.
 */
static void hash_rlp_field(const uint8_t *buf, size_t size)
{
	hash_rlp_length(size, buf[0]);
	/* FIXME: this special case should be handled more nicely */
	if (!(size == 1 && buf[0] <= 0x7f)) {
		hash_data(buf, size);
	}
}

static void send_request_chunk(void)
{
	resp.has_data_length = true;
	resp.data_length = data_left <= 1024 ? data_left : 1024;
	msg_write(MessageType_MessageType_EthereumTxRequest, &resp);
}

static void send_signature(void)
{
	keccak_Final(&keccak_ctx, hash);
	uint8_t v;
	if (ecdsa_sign_digest(&secp256k1, privkey, hash, sig, &v) != 0) {
		fsm_sendFailure(FailureType_Failure_Other, "Signing failed");
		ethereum_signing_abort();
		return;
	}

	memset(privkey, 0, sizeof(privkey));

	/* Send back the result */
	resp.has_data_length = false;

	resp.has_signature_v = true;
	resp.signature_v = v + 27;

	resp.has_signature_r = true;
	resp.signature_r.size = 32;
	memcpy(resp.signature_r.bytes, sig, 32);

	resp.has_signature_s = true;
	resp.signature_s.size = 32;
	memcpy(resp.signature_s.bytes, sig + 32, 32);

	msg_write(MessageType_MessageType_EthereumTxRequest, &resp);

	ethereum_signing_abort();
}

static void layoutEthereumConfirmTx(const uint8_t *to, const uint8_t *value)
{
	static bignum256 val;
	static uint16_t num[26];
	static uint8_t last_used = 0;
	if (value) {
		bn_read_be(value, &val);
	} else {
		bn_zero(&val);
	}
	for (int i = 0; i < 26; i++) {
		bn_divmod1000(&val, (uint32_t *)&(num[i]));
		if (num[i] > 0) {
			last_used = i;
		}
	}

	static char _value[25] = {0};
	const char *value_ptr = _value;

	if (last_used < 3) {
		// value is smaller than 1e9 wei => show value in wei
		_value[0] = '0' + (num[2] / 100) % 10;
		_value[1] = '0' + (num[2] / 10) % 10;
		_value[2] = '0' + (num[2]) % 10;
		_value[3] = '0' + (num[1] / 100) % 10;
		_value[4] = '0' + (num[1] / 10) % 10;
		_value[5] = '0' + (num[1]) % 10;
		_value[6] = '0' + (num[0] / 100) % 10;
		_value[7] = '0' + (num[0] / 10) % 10;
		_value[8] = '0' + (num[0]) % 10;
		strlcpy(_value + 9, " wei", sizeof(_value) - 9);
	} else if (last_used < 9) {
		// value is bigger than 1e9 wei and smaller than 1e9 ETH => show value in ETH
		_value[0] = '0' + (num[8] / 100) % 10;
		_value[1] = '0' + (num[8] / 10) % 10;
		_value[2] = '0' + (num[8]) % 10;
		_value[3] = '0' + (num[7] / 100) % 10;
		_value[4] = '0' + (num[7] / 10) % 10;
		_value[5] = '0' + (num[7]) % 10;
		_value[6] = '0' + (num[6] / 100) % 10;
		_value[7] = '0' + (num[6] / 10) % 10;
		_value[8] = '0' + (num[6]) % 10;
		_value[9] = '.';
		_value[10] = '0' + (num[5] / 100) % 10;
		_value[11] = '0' + (num[5] / 10) % 10;
		_value[12] = '0' + (num[5]) % 10;
		_value[13] = '0' + (num[4] / 100) % 10;
		_value[14] = '0' + (num[4] / 10) % 10;
		_value[15] = '0' + (num[4]) % 10;
		_value[16] = '0' + (num[3] / 100) % 10;
		_value[17] = '0' + (num[3] / 10) % 10;
		_value[18] = '0' + (num[3]) % 10;
		strlcpy(_value + 19, " ETH", sizeof(_value) - 19);
	} else {
		// value is bigger than 1e9 ETH => won't fit on display (probably won't happen unless you are Vitalik)
		strlcpy(_value, "more than a billion ETH", sizeof(_value));
	}

	value_ptr = _value;
	while (*value_ptr == '0') { // skip leading zeroes
		value_ptr++;
	}

	static char _to1[21] = {0};
	static char _to2[21] = {0};

	if (to) {
		data2hex(to, 10, _to1);
		data2hex(to + 10, 10, _to2);
	} else {
		strlcpy(_to1, "no recipient", sizeof(_to1));
		strlcpy(_to2, "", sizeof(_to2));
	}

	layoutDialogSwipe(DIALOG_ICON_QUESTION,
		"Cancel",
		"Confirm",
		NULL,
		"Really send",
		value_ptr,
		"from your wallet?",
		"To:",
		_to1,
		_to2
	);
}

/*
 * RLP fields:
 * - nonce (0 .. 32)
 * - gas_price (0 .. 32)
 * - gas_limit (0 .. 32)
 * - to (0, 20)
 * - value (0 .. 32)
 * - data (0 ..)
 */

void ethereum_signing_init(EthereumSignTx *msg, const HDNode *node)
{
	signing = true;
	sha3_256_Init(&keccak_ctx);

	memset(&resp, 0, sizeof(EthereumTxRequest));

	if (msg->has_data_length) {
		if (msg->data_length == 0) {
			fsm_sendFailure(FailureType_Failure_Other, "Invalid data length provided");
			ethereum_signing_abort();
			return;
		}
		if (!msg->has_data_initial_chunk || msg->data_initial_chunk.size == 0) {
			fsm_sendFailure(FailureType_Failure_Other, "Data length provided, but no initial chunk");
			ethereum_signing_abort();
			return;
		}
		if (msg->data_initial_chunk.size > msg->data_length) {
			fsm_sendFailure(FailureType_Failure_Other, "Invalid size of initial chunk");
			ethereum_signing_abort();
			return;
		}
	}

	layoutEthereumConfirmTx(msg->has_to ? msg->to.bytes : NULL, msg->has_value ? msg->value.bytes : NULL);
	if (!protectButton(ButtonRequestType_ButtonRequest_SignTx, false)) {
		fsm_sendFailure(FailureType_Failure_ActionCancelled, "Signing cancelled by user");
		ethereum_signing_abort();
		return;
	}

	/* Stage 1: Calculate total RLP length */
	int total_rlp_length = 0;
	int total_data_length = 0;

	if (msg->has_data_length) {
		total_data_length = msg->data_length;
	}

	layoutProgress("Signing", 1);

	if (msg->has_nonce) {
		total_rlp_length += rlp_calculate_length(msg->nonce.size, msg->nonce.bytes[0]);
	} else {
		total_rlp_length++;
	}

	layoutProgress("Signing", 2);

	if (msg->has_gas_price) {
		total_rlp_length += rlp_calculate_length(msg->gas_price.size, msg->gas_price.bytes[0]);
	} else {
		total_rlp_length++;
	}

	layoutProgress("Signing", 3);

	if (msg->has_gas_limit) {
		total_rlp_length += rlp_calculate_length(msg->gas_limit.size, msg->gas_limit.bytes[0]);
	} else {
		total_rlp_length++;
	}

	layoutProgress("Signing", 4);

	if (msg->has_to) {
		total_rlp_length += rlp_calculate_length(msg->to.size, msg->to.bytes[0]);
	} else {
		total_rlp_length++;
	}

	layoutProgress("Signing", 5);

	if (msg->has_value) {
		total_rlp_length += rlp_calculate_length(msg->value.size, msg->value.bytes[0]);
	} else {
		total_rlp_length++;
	}

	layoutProgress("Signing", 6);

	if (msg->has_data_initial_chunk) {
		total_rlp_length += rlp_calculate_length(total_data_length, msg->data_initial_chunk.bytes[0]);
	} else {
		total_rlp_length++;
	}

	layoutProgress("Signing", 7);

	/* Stage 2: Store header fields */
	hash_rlp_list_length(total_rlp_length);

	layoutProgress("Signing", 8);

	if (msg->has_nonce) {
		hash_rlp_field(msg->nonce.bytes, msg->nonce.size);
	} else {
		hash_rlp_length(1, 0);
	}

	if (msg->has_gas_price) {
		hash_rlp_field(msg->gas_price.bytes, msg->gas_price.size);
	} else {
		hash_rlp_length(1, 0);
	}

	if (msg->has_gas_limit) {
		hash_rlp_field(msg->gas_limit.bytes, msg->gas_limit.size);
	} else {
		hash_rlp_length(1, 0);
	}

	if (msg->has_to) {
		hash_rlp_field(msg->to.bytes, msg->to.size);
	} else {
		hash_rlp_length(1, 0);
	}

	if (msg->has_value) {
		hash_rlp_field(msg->value.bytes, msg->value.size);
	} else {
		hash_rlp_length(1, 0);
	}

	if (msg->has_data_initial_chunk) {
		hash_rlp_length(total_data_length, msg->data_initial_chunk.bytes[0]);
		hash_data(msg->data_initial_chunk.bytes, msg->data_initial_chunk.size);
	} else {
		hash_rlp_length(1, 0);
	}

	layoutProgress("Signing", 9);

	/* FIXME: probably this shouldn't be done here, but at a later stage */
	memcpy(privkey, node->private_key, 32);

	if (msg->has_data_length && msg->data_length > 0) {
		layoutProgress("Signing", 20);
		data_left = msg->data_length - msg->data_initial_chunk.size;
		send_request_chunk();
	} else {
		layoutProgress("Signing", 50);
		send_signature();
	}
}

void ethereum_signing_txack(EthereumTxAck *tx)
{
	if (!signing) {
		fsm_sendFailure(FailureType_Failure_UnexpectedMessage, "Not in Signing mode");
		layoutHome();
		return;
	}

	if (data_left > 0 && (!tx->has_data_chunk || tx->data_chunk.size == 0)) {
		fsm_sendFailure(FailureType_Failure_Other, "Empty data chunk received");
		ethereum_signing_abort();
		return;
	}

	hash_data(tx->data_chunk.bytes, tx->data_chunk.size);

	data_left -= tx->data_chunk.size;

	if (data_left > 0) {
		send_request_chunk();
	} else {
		send_signature();
	}
}

void ethereum_signing_abort(void)
{
	if (signing) {
		memset(privkey, 0, sizeof(privkey));
		layoutHome();
		signing = false;
	}
}
