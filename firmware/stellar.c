/*
 * This file is part of the TREZOR project.
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
 *
 * Stellar signing has the following workflow:
 *  1. Client sends first 1024 bytes of the transaction
 *  2. Trezor parses the transaction header and confirms the details with the user
 *  3. Trezor responds to the client with an offset for where to send the next chunk of bytes
 *  4. Client sends next 1024 bytes starting at <offset>
 *  5. Trezor parses and confirms the next operation
 *  6. Trezor responds with either an offset for the next operation or a signature
 */

#include <stdbool.h>
#include <time.h>
#include "messages.h"
#include "messages.pb.h"
#include "stellar.h"
#include "bip32.h"
#include "crypto.h"
#include "layout2.h"
#include "gettext.h"
#include "bignum.h"
#include "oled.h"
#include "base32.h"
#include "storage.h"
#include "fsm.h"
#include "protect.h"
#include "util.h"

static bool stellar_signing = false;
static StellarTransaction stellar_activeTx;

/*
 * Starts the signing process and parses the transaction header
 */
void stellar_signingInit(StellarSignTx *msg)
{
    memset(&stellar_activeTx, 0, sizeof(StellarTransaction));
    stellar_signing = true;
    // Initialize signing context
    sha256_Init(&(stellar_activeTx.sha256_ctx));

    // Calculate sha256 for network passphrase
    // max length defined in messages.options
    uint8_t network_hash[32];
    sha256_Raw((uint8_t *)msg->network_passphrase, strnlen(msg->network_passphrase, 1024), network_hash);

    uint8_t tx_type_bytes[4] = { 0x00, 0x00, 0x00, 0x02 };

    // Copy some data into the active tx
    stellar_activeTx.num_operations = msg->num_operations;

    // Start building what will be signed:
    // sha256 of:
    //  sha256(network passphrase)
    //  4-byte unsigned big-endian int type constant (2 for tx)
    //  remaining bytes are operations added in subsequent messages
    stellar_hashupdate_bytes(network_hash, sizeof(network_hash));
    stellar_hashupdate_bytes(tx_type_bytes, sizeof(tx_type_bytes));

    // Public key comes from deriving the specified account path (we ignore the one sent by the client)
    uint8_t bytes_pubkey[32];
    stellar_getPubkeyAtIndex(msg->index, bytes_pubkey, sizeof(bytes_pubkey));
    memcpy(&(stellar_activeTx.account_id), bytes_pubkey, sizeof(stellar_activeTx.account_id));
    memcpy(&(stellar_activeTx.account_index), &(msg->index), sizeof(stellar_activeTx.account_index));

    // Hash: public key
    stellar_hashupdate_address(bytes_pubkey);

    // Hash: fee
    stellar_hashupdate_uint32(msg->fee);

    // Hash: sequence number
    stellar_hashupdate_uint64(msg->sequence_number);

    // Timebounds are only present if timebounds_start or timebounds_end is non-zero
    uint8_t has_timebounds = (msg->timebounds_start > 0 || msg->timebounds_end > 0);
    if (has_timebounds) {
        // Hash: the "has timebounds?" boolean
        stellar_hashupdate_bool(true);

        // Timebounds are sent as uint32s since that's all we can display, but they must be hashed as
        // 64-bit values
        stellar_hashupdate_uint32(0);
        stellar_hashupdate_uint32(msg->timebounds_start);

        stellar_hashupdate_uint32(0);
        stellar_hashupdate_uint32(msg->timebounds_end);
    }
    // No timebounds, hash a false boolean
    else {
        stellar_hashupdate_bool(false);
    }

    // Hash: memo
    stellar_hashupdate_uint32(msg->memo_type);
    switch (msg->memo_type) {
        // None, nothing else to do
        case 0:
            break;
        // Text: 4 bytes (size) + up to 28 bytes
        case 1:
            stellar_hashupdate_string((unsigned char*)&(msg->memo_text), strnlen(msg->memo_text, 28));
            break;
        // ID (8 bytes, uint64)
        case 2:
            stellar_hashupdate_uint64(msg->memo_id);
            break;
        // Hash and return are the same data structure (32 byte tx hash)
        case 3:
        case 4:
            stellar_hashupdate_bytes(msg->memo_hash.bytes, msg->memo_hash.size);
            break;
        default:
            break;
    }

    // Hash: number of operations
    stellar_hashupdate_uint32(msg->num_operations);

    // Determine what type of network this transaction is for
    if (strncmp("Public Global Stellar Network ; September 2015", msg->network_passphrase, 1024) == 0) {
        stellar_activeTx.network_type = 1;
    }
    else if (strncmp("Test SDF Network ; September 2015", msg->network_passphrase, 1024) == 0) {
        stellar_activeTx.network_type = 2;
    }
    else {
        stellar_activeTx.network_type = 3;
    }
}

/*
 * Adds an operation to the current transaction by parsing the StellarTxOpAck message
 */
void stellar_addOperation(StellarTxOpAck *msg)
{
    if (!stellar_signing) {
        fsm_sendFailure(FailureType_Failure_UnexpectedMessage, _("Not in Stellar signing mode"));
        layoutHome();
        return;
    }

    // Source account is optional
    // Prompt the user for additional verification if one is present
    if (msg->source_account.size > 0) {
        const char **str_addr_rows = stellar_lineBreakAddress(msg->source_account.bytes);

        stellar_layoutTransactionDialog(
            _("Op src account OK?"),
            NULL,
            str_addr_rows[0],
            str_addr_rows[1],
            str_addr_rows[2]
        );
        if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
            stellar_signingAbort();
            return;
        }

        // Hash: source account
        stellar_hashupdate_address(msg->source_account.bytes);
    }
    else {
        stellar_hashupdate_bool(false);
    }

    // Hash: operation type
    stellar_hashupdate_uint32(msg->type);

    // Create Account
    if (msg->type == 0) {
        stellar_confirmCreateAccountOp(msg);
    }
    // Payment
    if (msg->type == 1) {
        stellar_confirmPaymentOp(msg);
    }

    // If the last operation was confirmed, update the hash with 4 null bytes.
    // These are for the currently reserved union at the end of the transaction envelope
    if (stellar_allOperationsConfirmed()) {
        stellar_hashupdate_uint32(0);
    }
}

void stellar_confirmCreateAccountOp(StellarTxOpAck *msg)
{
    const char **str_addr_rows = stellar_lineBreakAddress(msg->destination_account.bytes);

    // Hash: address
    stellar_hashupdate_address(msg->destination_account.bytes);
    // Hash: starting amount
    stellar_hashupdate_uint64(msg->amount);

    // Amount being funded
    char str_amount_line[32];
    char str_amount[32];
    stellar_format_stroops(msg->amount, str_amount, sizeof(str_amount));

    strlcpy(str_amount_line, _("With "), sizeof(str_amount_line));
    strlcat(str_amount_line, str_amount, sizeof(str_amount_line));
    strlcat(str_amount_line, _(" XLM"), sizeof(str_amount_line));

    stellar_layoutTransactionDialog(
        _("Create account: "),
        str_addr_rows[0],
        str_addr_rows[1],
        str_addr_rows[2],
        str_amount_line
    );
    if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
        stellar_signingAbort();
        return;
    }

    // At this point, the operation is confirmed
    stellar_activeTx.confirmed_operations++;
}

void stellar_confirmPaymentOp(StellarTxOpAck *msg)
{
    const char **str_addr_rows = stellar_lineBreakAddress(msg->destination_account.bytes);

    // To: G...
    char str_to[32];
    strlcpy(str_to, _("To: "), sizeof(str_to));
    strlcat(str_to, str_addr_rows[0], sizeof(str_to));

    // Hash: destination
    stellar_hashupdate_address(msg->destination_account.bytes);

    // Asset
    char str_asset_row[32];
    char str_asset_code[12 + 1];
    // Full asset issuer string
    char str_asset_issuer[56+1];
    // truncated asset issuer, G1234
    char str_asset_issuer_trunc[5 + 1];

    // Native asset
    if (msg->asset.type == 0) {
        strlcpy(str_asset_row, _("XLM (native asset)"), sizeof(str_asset_row));

        // Hash: asset type
        stellar_hashupdate_uint32(msg->asset.type);
    }
    // 4-character custom
    if (msg->asset.type == 1) {
        memcpy(str_asset_code, msg->asset.code, 4);
        strlcpy(str_asset_row, str_asset_code, sizeof(str_asset_row));

        // Hash: asset code
        stellar_hashupdate_bytes((uint8_t *)(msg->asset.code), 4);
    }
    if (msg->asset.type == 2) {
        memcpy(str_asset_code, msg->asset.code, 12);
        strlcpy(str_asset_row, str_asset_code, sizeof(str_asset_row));

        // Hash: asset code
        stellar_hashupdate_bytes((uint8_t *)(msg->asset.code), 12);
    }
    // Issuer is read the same way for both types of custom assets
    if (msg->asset.type == 1 || msg->asset.type == 2) {
        stellar_publicAddressAsStr(msg->asset.issuer.bytes, str_asset_issuer, sizeof(str_asset_issuer));
        memcpy(str_asset_issuer_trunc, str_asset_issuer, 5);

        // Hash: asset issuer
        stellar_hashupdate_bytes(msg->asset.issuer.bytes, msg->asset.issuer.size);

        strlcat(str_asset_row, _(" ("), sizeof(str_asset_row));
        strlcat(str_asset_row, str_asset_issuer_trunc, sizeof(str_asset_row));
        strlcat(str_asset_row, _(")"), sizeof(str_asset_row));
    }

    char str_pay_amount[32];
    char str_amount[32];
    stellar_format_stroops(msg->amount, str_amount, sizeof(str_amount));

    // Hash: amount
    // todo: amount can be signed?
    stellar_hashupdate_uint64(msg->amount);

    strlcpy(str_pay_amount, _("Pay "), sizeof(str_pay_amount));
    strlcat(str_pay_amount, str_amount, sizeof(str_pay_amount));

    stellar_layoutTransactionDialog(
        str_pay_amount,
        str_asset_row,
        str_to,
        str_addr_rows[1],
        str_addr_rows[2]
    );
    if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
        stellar_signingAbort();
        return;
    }

    // At this point, the operation is confirmed
    stellar_activeTx.confirmed_operations++;
}

void stellar_signingAbort()
{
    stellar_signing = false;
    fsm_sendFailure(FailureType_Failure_ActionCancelled, NULL);
    layoutHome();
}

uint32_t stellar_getXdrOffset()
{
    return stellar_activeTx.xdr_offset;
}

uint8_t stellar_allOperationsConfirmed()
{
    return stellar_activeTx.confirmed_operations == stellar_activeTx.num_operations;
}

StellarTransaction *stellar_getActiveTx()
{
    return &stellar_activeTx;
}

/*
 * Calculates and sets the signature for the active transaction
 */
void stellar_getSignatureForActiveTx(uint8_t *out_signature)
{
    HDNode *node = stellar_deriveNode(stellar_activeTx.account_index);

    // Signature is the ed25519 detached signature of the sha256 of all the bytes
    // that have been read so far
    uint8_t to_sign[32];
    sha256_Final(&(stellar_activeTx.sha256_ctx), to_sign);

    uint8_t signature[64];
    ed25519_sign(to_sign, sizeof(to_sign), node->private_key, node->public_key + 1, signature);

    memcpy(out_signature, signature, sizeof(signature));
}

/*
 * Returns number (representing stroops) formatted as XLM
 * For example, if number has value 1000000000 then it will be returned as "100.0"
 */
void stellar_format_stroops(uint64_t number, char *out, size_t outlen)
{
    bn_format_uint64(number, NULL, NULL, 7, 0, false, out, outlen);
}

/*
 * Returns a uint32 formatted as a string
 */
void stellar_format_uint32(uint64_t number, char *out, size_t outlen)
{
    bignum256 bn_number;
    bn_read_uint32(number, &bn_number);
    bn_format(&bn_number, NULL, NULL, 0, 0, false, out, outlen);
}

/*
 * Returns a uint64 formatted as a string
 */
void stellar_format_uint64(uint64_t number, char *out, size_t outlen)
{
    bn_format_uint64(number, NULL, NULL, 0, 0, false, out, outlen);
}

/*
 * Breaks a 56 character address into 3 lines of lengths 16, 20, 20
 * This is to allow a small label to be prepended to the first line
 */
const char **stellar_lineBreakAddress(uint8_t *addrbytes)
{
    char str_fulladdr[56+1];
    static char rows[3][20+1];

    memset(rows, 0, sizeof(rows));

    // get full address string
    stellar_publicAddressAsStr(addrbytes, str_fulladdr, sizeof(str_fulladdr));

    // Break it into 3 lines
    strlcpy(rows[0], str_fulladdr + 0, 17);
    strlcpy(rows[1], str_fulladdr + 16, 21);
    strlcpy(rows[2], str_fulladdr + 16 + 20, 21);

    static const char *ret[3] = { rows[0], rows[1], rows[2] };
    return ret;
}

size_t stellar_publicAddressAsStr(uint8_t *bytes, char *out, size_t outlen)
{
    // version + key bytes + checksum
    uint8_t keylen = 1 + 32 + 2;
    uint8_t bytes_full[keylen];
    bytes_full[0] = 6 << 3; // 'G'

    memcpy(bytes_full + 1, bytes, 32);

    // Last two bytes are the checksum
    uint16_t checksum = stellar_crc16(bytes_full, 33);
    bytes_full[keylen-2] = checksum & 0x00ff;
    bytes_full[keylen-1] = (checksum>>8) & 0x00ff;

    base32_encode(bytes_full, keylen, out, outlen, BASE32_ALPHABET_RFC4648);

    // Public key will always be 56 characters
    return 56;
}

/*
 * CRC16 implementation compatible with the Stellar version
 * Ported from this implementation: http://introcs.cs.princeton.edu/java/61data/CRC16CCITT.java.html
 * Initial value changed to 0x0000 to match Stellar
 */
uint16_t stellar_crc16(uint8_t *bytes, uint32_t length)
{
    // Calculate checksum for existing bytes
    uint16_t crc = 0x0000;
    uint16_t polynomial = 0x1021;
    uint32_t i;
    uint8_t bit;
    uint8_t byte;
    uint8_t bitidx;
    uint8_t c15;

    for (i=0; i < length; i++) {
        byte = bytes[i];
        for (bitidx=0; bitidx < 8; bitidx++) {
            bit = ((byte >> (7 - bitidx) & 1) == 1);
            c15 = ((crc >> 15 & 1) == 1);
            crc <<= 1;
            if (c15 ^ bit) crc ^= polynomial;
        }
    }

    return crc & 0xffff;
}

/*
 * Writes 32-byte public key to out
 */
void stellar_getPubkeyAtIndex(uint32_t index, uint8_t *out, size_t outlen)
{
    if (outlen < 32) return;

    HDNode *node = stellar_deriveNode(index);

    memcpy(out, node->public_key + 1, outlen);
}

/*
 * Derives the HDNode at the given index
 * The prefix for this is m/44'/148'/index'
 */
HDNode *stellar_deriveNode(uint32_t index)
{
    static CONFIDENTIAL HDNode node;
    const char *curve = "ed25519";

    // Derivation path for Stellar is m/44'/148'/index'
    uint32_t address_n[3];
    address_n[0] = 0x80000000 | 44;
    address_n[1] = 0x80000000 | 148;
    address_n[2] = 0x80000000 | index;

    // Device not initialized, passphrase request cancelled, or unsupported curve
    if (!storage_getRootNode(&node, curve, true)) {
        return 0;
    }
    // Failed to derive private key
    if (hdnode_private_ckd_cached(&node, address_n, 3, NULL) == 0) {
        return 0;
    }

    hdnode_fill_public_key(&node);

    return &node;
}

void stellar_hashupdate_uint32(uint32_t value)
{
    // Ensure uint32 is big endian
#if BYTE_ORDER == LITTLE_ENDIAN
    REVERSE32(value, value);
#endif

    // Byte values must be hashed as big endian
    uint8_t data[4];
    data[3] = (value >> 24) & 0xFF;
    data[2] = (value >> 16) & 0xFF;
    data[1] = (value >> 8)  & 0xFF;
    data[0] = value         & 0xFF;

    stellar_hashupdate_bytes(data, sizeof(data));
}

void stellar_hashupdate_uint64(uint64_t value)
{
    // Ensure uint64 is big endian
#if BYTE_ORDER == LITTLE_ENDIAN
    REVERSE64(value, value);
#endif

    // Byte values must be hashed as big endian
    uint8_t data[8];
    data[7] = (value >> 56) & 0xFF;
    data[6] = (value >> 48) & 0xFF;
    data[5] = (value >> 40) & 0xFF;
    data[4] = (value >> 32) & 0xFF;
    data[3] = (value >> 24) & 0xFF;
    data[2] = (value >> 16) & 0xFF;
    data[1] = (value >> 8)  & 0xFF;
    data[0] = value         & 0xFF;

    stellar_hashupdate_bytes(data, sizeof(data));
}

void stellar_hashupdate_bool(bool value)
{
    if (value) {
        stellar_hashupdate_uint32(1);
    }
    else {
        stellar_hashupdate_uint32(0);
    }
}

void stellar_hashupdate_string(uint8_t *data, size_t len)
{
    // Hash the length of the string
    stellar_hashupdate_uint32((uint32_t)len);

    // Hash the raw bytes of the string
    stellar_hashupdate_bytes(data, len);
}

void stellar_hashupdate_address(uint8_t *address_bytes)
{
    // First 4 bytes of an address are the type. There's only one type (0)
    stellar_hashupdate_uint32(0);

    // Remaining part of the address is 32 bytes
    stellar_hashupdate_bytes(address_bytes, 32);
}

void stellar_hashupdate_bytes(uint8_t *data, size_t len)
{
    sha256_Update(&(stellar_activeTx.sha256_ctx), data, len);
}

/*
 * Reads stellar_activeTx and displays a summary of the overall transaction
 */
void stellar_layoutTransactionSummary(StellarSignTx *msg)
{
    char str_lines[5][32];
    memset(str_lines, 0, sizeof(str_lines));

    char str_fee[12];
    char str_num_ops[12];

    // Will be set to true for some large hashes that don't fit on one screen
    uint8_t needs_memo_hash_confirm = 0;

    // Format the fee
    stellar_format_stroops(msg->fee, str_fee, sizeof(str_fee));

    strlcpy(str_lines[0], _("Fee: "), sizeof(str_lines[0]));
    strlcat(str_lines[0], str_fee, sizeof(str_lines[0]));
    strlcat(str_lines[0], _(" XLM"), sizeof(str_lines[0]));

    // add in numOperations
    stellar_format_uint32(msg->num_operations, str_num_ops, sizeof(str_num_ops));

    strlcat(str_lines[0], _(" ("), sizeof(str_lines[0]));
    strlcat(str_lines[0], str_num_ops, sizeof(str_lines[0]));
    if (msg->num_operations == 1) {
        strlcat(str_lines[0], _(" op)"), sizeof(str_lines[0]));
    } else {
        strlcat(str_lines[0], _(" ops)"), sizeof(str_lines[0]));
    }

    // Display full address being used to sign transaction
    const char **str_addr_rows = stellar_lineBreakAddress(stellar_activeTx.account_id);

    stellar_layoutTransactionDialog(
        str_lines[0],
        _("Signing with:"),
        str_addr_rows[0],
        str_addr_rows[1],
        str_addr_rows[2]
    );
    if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
        stellar_signingAbort();
        return;
    }

    // Reset lines for displaying memo
    memset(str_lines, 0, sizeof(str_lines));

    // Memo: none
    if (stellar_activeTx.memo_type == 0) {
        strlcpy(str_lines[0], _("[No Memo]"), sizeof(str_lines[0]));
    }
    // Memo: text
    if (msg->memo_type == 1) {
        strlcpy(str_lines[0], _("Memo (TEXT)"), sizeof(str_lines[0]));

        // Split 28-character string into two lines of 19 / 9
        // todo: word wrap method?
        strlcpy(str_lines[1], (const char*)msg->memo_text, 19 + 1);
        strlcpy(str_lines[2], (const char*)(msg->memo_text + 19), 9 + 1);
    }
    // Memo: ID
    if (stellar_activeTx.memo_type == 2) {
        strlcpy(str_lines[0], _("Memo (ID)"), sizeof(str_lines[0]));
        stellar_format_uint64(msg->memo_id, str_lines[1], sizeof(str_lines[1]));
    }
    // Memo: hash
    if (stellar_activeTx.memo_type == 3) {
        needs_memo_hash_confirm = 1;
        strlcpy(str_lines[0], _("Memo (HASH)"), sizeof(str_lines[0]));
    }
    // Memo: return
    if (stellar_activeTx.memo_type == 4) {
        needs_memo_hash_confirm = 1;
        strlcpy(str_lines[0], _("Memo (RETURN)"), sizeof(str_lines[0]));
    }

    if (needs_memo_hash_confirm) {
        data2hex(msg->memo_hash.bytes +  0, 8, str_lines[1]);
        data2hex(msg->memo_hash.bytes +  8, 8, str_lines[2]);
        data2hex(msg->memo_hash.bytes + 16, 8, str_lines[3]);
        data2hex(msg->memo_hash.bytes + 24, 8, str_lines[4]);
    }

    stellar_layoutTransactionDialog(
        str_lines[0],
        str_lines[1],
        str_lines[2],
        str_lines[3],
        str_lines[4]
    );
    if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
        stellar_signingAbort();
        return;
    }

    // Verify timebounds, if present
    memset(str_lines, 0, sizeof(str_lines));

    // Timebound: lower
    if (msg->timebounds_start || msg->timebounds_end) {
        time_t timebound;
        char str_timebound[32];
        const struct tm *tm;

        timebound = (time_t)msg->timebounds_start;
        strlcpy(str_lines[0], _("Valid from:"), sizeof(str_lines[0]));
        if (timebound) {
            tm = gmtime(&timebound);
            strftime(str_timebound, sizeof(str_timebound), "%F %T (UTC)", tm);
            strlcpy(str_lines[1], str_timebound, sizeof(str_lines[1]));
        }
        else {
            strlcpy(str_lines[1], _("[no restriction]"), sizeof(str_lines[1]));
        }

        // Reset for timebound_max
        memset(str_timebound, 0, sizeof(str_timebound));

        timebound = (time_t)msg->timebounds_end;
        strlcpy(str_lines[0], _("Valid from:"), sizeof(str_lines[0]));
        if (timebound) {
            tm = gmtime(&timebound);
            strftime(str_timebound, sizeof(str_timebound), "%F %T (UTC)", tm);
            strlcpy(str_lines[1], str_timebound, sizeof(str_lines[1]));
        }
        else {
            strlcpy(str_lines[1], _("[no restriction]"), sizeof(str_lines[1]));
        }
    }

    if (msg->timebounds_start || msg->timebounds_end) {
        stellar_layoutTransactionDialog(
            _("Confirm Time Bounds"),
            str_lines[0],
            str_lines[1],
            str_lines[2],
            str_lines[3]
        );
        if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
            stellar_signingAbort();
            return;
        }
    }
}

/*
 * Main dialog helper method. Allows displaying 5 lines.
 * A title showing the account being used to sign is always displayed.
 */
void stellar_layoutTransactionDialog(const char *line1, const char *line2, const char *line3, const char *line4, const char *line5)
{
    // Start with some initial padding and use these to track position as rendering moves down the screen
    int offset_x = 1;
    int offset_y = 1;
    int line_height = 9;

    char str_account_index[12];
    char str_pubaddr_truncated[6]; // G???? + null

    layoutLast = layoutDialogSwipe;
    layoutSwipe();
    oledClear();

    // Load up public address
    char str_pubaddr[56+1];
    memset(str_pubaddr, 0, sizeof(str_pubaddr));
    stellar_publicAddressAsStr(stellar_activeTx.account_id, str_pubaddr, sizeof(str_pubaddr));
    memcpy(str_pubaddr_truncated, str_pubaddr, 5);

    // Format account index
    stellar_format_uint32(stellar_activeTx.account_index + 1, str_account_index, sizeof(str_account_index));

    // Header
    // Ends up as: Signing with #1 (GABCD)
    char str_header[32];
    memset(str_header, 0, sizeof(str_header));
    strlcpy(str_header, _("Signing with #"), sizeof(str_header));
    strlcat(str_header, str_account_index, sizeof(str_header));
    strlcat(str_header, _(" ("), sizeof(str_header));
    strlcat(str_header, str_pubaddr_truncated, sizeof(str_header));
    strlcat(str_header, _(")"), sizeof(str_header));

    oledDrawString(offset_x, offset_y, str_header);
    offset_y += line_height;
    // Invert color on header
    oledInvert(0, 0, OLED_WIDTH, offset_y - 2);

    // Dialog contents begin
    if (line1) {
        oledDrawString(offset_x, offset_y, line1);
        offset_y += line_height;
    }
    if (line2) {
        oledDrawString(offset_x, offset_y, line2);
        offset_y += line_height;
    }
    if (line3) {
        oledDrawString(offset_x, offset_y, line3);
        offset_y += line_height;
    }
    if (line4) {
        oledDrawString(offset_x, offset_y, line4);
        offset_y += line_height;
    }
    if (line5) {
        oledDrawString(offset_x, offset_y, line5);
        offset_y += line_height;
    }

    // Cancel button
    oledDrawString(1, OLED_HEIGHT - 8, "\x15");
    oledDrawString(fontCharWidth('\x15') + 3, OLED_HEIGHT - 8, "Cancel");
    oledInvert(0, OLED_HEIGHT - 9, fontCharWidth('\x15') + oledStringWidth("Cancel") + 2, OLED_HEIGHT - 1);

    // Warnings (drawn centered between the buttons
    if (stellar_activeTx.network_type == 2) {
        // Warning: testnet
        oledDrawStringCenter(OLED_HEIGHT - 8, "WRN:TN");
    }
    if (stellar_activeTx.network_type == 3) {
        // Warning: private network
        oledDrawStringCenter(OLED_HEIGHT - 8, "WRN:PN");
    }


    // Next / confirm button
    oledDrawString(OLED_WIDTH - fontCharWidth('\x06') - 1, OLED_HEIGHT - 8, "\x06");
    oledDrawString(OLED_WIDTH - oledStringWidth("Next") - fontCharWidth('\x06') - 3, OLED_HEIGHT - 8, "Next");
    oledInvert(OLED_WIDTH - oledStringWidth("Next") - fontCharWidth('\x06') - 4, OLED_HEIGHT - 9, OLED_WIDTH - 1, OLED_HEIGHT - 1);

    oledRefresh();
}

void stellar_layoutStellarGetPublicKey(uint32_t index)
{
    char str_title[32];
    char str_index[12];

    stellar_format_uint32(index+1, str_index, sizeof(str_index));

    // Share account #100?
    strlcpy(str_title, _("Share account #"), sizeof(str_title));
    strlcat(str_title, str_index, sizeof(str_title));
    strlcat(str_title, _("?"), sizeof(str_title));

    // Derive node and calculate address
    uint8_t pubkey_bytes[32];
    stellar_getPubkeyAtIndex(index, pubkey_bytes, sizeof(pubkey_bytes));
    const char **str_addr_rows = stellar_lineBreakAddress(pubkey_bytes);

    layoutDialogSwipe(&bmp_icon_question, _("Cancel"), _("Confirm"), _("Share public account ID?"),
        str_title,
        str_addr_rows[0],
        str_addr_rows[1],
        str_addr_rows[2],
        NULL, NULL
        );
    if (!protectButton(ButtonRequestType_ButtonRequest_ProtectCall, false)) {
        fsm_sendFailure(FailureType_Failure_ActionCancelled, NULL);
        layoutHome();
        return;
    }
}