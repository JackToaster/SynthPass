-- SynthPass BLE Advertising Post-Dissector

local synthpass = Proto("synthpass", "SynthPass BLE Protocol")

-- =========================
-- Configuration
-- =========================
local SYNTHPASS_MAC = "33:3a:33:3a:33:3a"

-- =========================
-- Fields
-- =========================
local f_sender_uid = ProtoField.uint32("synthpass.sender_uid", "Sender UID", base.HEX)
local f_msg_type   = ProtoField.uint8 ("synthpass.msg_type", "Message Type", base.HEX, {
    [0x00] = "Broadcast",
    [0x01] = "Prox",
    [0x11] = "Prox Data",
    [0x21] = "Prox Data ACK",
    [0x02] = "Boop",
    [0x12] = "Boop Data",
    [0x22] = "Boop Data ACK",
    [0x03] = "Unboop"
})
local f_ref_rssi   = ProtoField.int8 ("synthpass.ref_rssi", "Reference RSSI (1m)", base.DEC)

local f_peer_uid   = ProtoField.uint32("synthpass.peer_uid", "Peer UID", base.HEX)
local f_rx_rssi    = ProtoField.int8 ("synthpass.rx_rssi", "Received RSSI", base.DEC)
local f_user_data  = ProtoField.bytes("synthpass.user_data", "User Data")
local f_ack_peer   = ProtoField.uint32("synthpass.ack_peer_uid", "Ack Peer UID", base.HEX)

synthpass.fields = {
    f_sender_uid,
    f_msg_type,
    f_ref_rssi,
    f_peer_uid,
    f_rx_rssi,
    f_user_data,
    f_ack_peer
}

local f_bt_addr = Field.new("bthci_evt.bd_addr")
local f_companyid = Field.new("btcommon.eir_ad.entry.company_id")
local f_mfg_data = Field.new("btcommon.eir_ad.entry.data")

-- =========================
-- Post-dissector
-- =========================
function synthpass.dissector(tvb, pinfo, tree)
    local bt_addr_fi = f_bt_addr()
    if not bt_addr_fi then
        print("no bt addr field")
        return
    end

    local addr = tostring(bt_addr_fi)
    if addr ~= SYNTHPASS_MAC then
        -- print("mac mismatch: " .. addr)
        return
    end

    local mfg_data = f_mfg_data()
    -- Locate manufacturer data fields
    if not mfg_data then
        print("no mfg fields :/")
        return
    end

    local companyid = f_companyid()
    -- Locate manufacturer data fields
    if not companyid then
        print("no company id field :/")
        return
    end

    local companyid_tvb = companyid.range:tvb()
    local data_tvb = mfg_data.range:tvb()
    -- print("id offset " .. companyid.offset .. " range " .. companyid.range)
    -- print("data offset " .. mfg_data.offset .. " range " .. mfg_data.range)

    -- print(companyid_tvb .. " / " .. data_tvb)
    local data_len = companyid_tvb:len() + data_tvb:len()
    if data_len < 6 then
        print("too short: " .. data_len .. " < 6")
        return
    end


    local sender_uid0 = companyid_tvb(0,2):le_uint()

    local sender_uid1 = data_tvb(0, 2):le_uint();


    local msg_type = data_tvb(2,1):uint()
    if msg_type > 0x22 then
        print("invalid type " .. msg_type)
        -- return
    end

    -- =========================
    -- Claimed packet
    -- =========================
    -- print("claimed!")
    pinfo.cols.protocol = "SynthPass"

    local subtree = tree:add(
        synthpass,
        data_tvb(),
        "SynthPass Protocol"
    )

    subtree:add(f_sender_uid, companyid_tvb(0,2):le_uint() + data_tvb(0,2):le_uint() * 65536)

    local offset = 2

    subtree:add(f_msg_type, data_tvb(offset,1)); offset = offset + 1
    subtree:add(f_ref_rssi, data_tvb(offset,1)); offset = offset + 1

    -- Message-specific decoding
    if msg_type == 0x01 or msg_type == 0x02 or msg_type == 0x03 then
        if data_tvb:len() >= offset + 5 then
            subtree:add_le(f_peer_uid, data_tvb(offset,4)); offset = offset + 4
            subtree:add(f_rx_rssi, data_tvb(offset,1))
        else
            print("Not enough data for message type")
        end

    elseif msg_type == 0x11 or msg_type == 0x12 then
        if data_tvb:len() >= offset + 4 then
            subtree:add_le(f_peer_uid, data_tvb(offset,4)); offset = offset + 4
            if data_tvb:len() > offset then
                subtree:add(f_user_data, data_tvb(offset))
            end
        
        else
            print("Not enough data for message type")
        end

    elseif msg_type == 0x21 or msg_type == 0x22 then
        if data_tvb:len() >= offset + 4 then
            subtree:add_le(f_ack_peer, data_tvb(offset,4))
        
        else
            print("Not enough data for message type")
        end
    end
end

-- =========================
-- Register post-dissector
-- =========================
register_postdissector(synthpass)
