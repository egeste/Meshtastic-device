
#include <Arduino.h>
#include <assert.h>

#include "FS.h"

#include "CryptoEngine.h"
#include "FSCommon.h"
#include "GPS.h"
#include "MeshRadio.h"
#include "NodeDB.h"
#include "PacketHistory.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "Router.h"
#include "configuration.h"
#include "error.h"
#include "mesh-pb-constants.h"
#include "meshwifi/meshwifi.h"
#include <pb_decode.h>
#include <pb_encode.h>

NodeDB nodeDB;

// we have plenty of ram so statically alloc this tempbuf (for now)
EXT_RAM_ATTR DeviceState devicestate;
MyNodeInfo &myNodeInfo = devicestate.my_node;
RadioConfig &radioConfig = devicestate.radio;
ChannelSettings &channelSettings = radioConfig.channel_settings;

/** The current change # for radio settings.  Starts at 0 on boot and any time the radio settings
 * might have changed is incremented.  Allows others to detect they might now be on a new channel.
 */
uint32_t radioGeneration;

/*
DeviceState versions used to be defined in the .proto file but really only this function cares.  So changed to a
#define here.
*/

#define DEVICESTATE_CUR_VER 11
#define DEVICESTATE_MIN_VER DEVICESTATE_CUR_VER

// FIXME - move this somewhere else
extern void getMacAddr(uint8_t *dmac);

/**
 *
 * Normally userids are unique and start with +country code to look like Signal phone numbers.
 * But there are some special ids used when we haven't yet been configured by a user.  In that case
 * we use !macaddr (no colons).
 */
User &owner = devicestate.owner;

static uint8_t ourMacAddr[6];

/**
 * The node number the user is currently looking at
 * 0 if none
 */
NodeNum displayedNodeNum;

/// A usable (but bigger) version of the channel name in the channelSettings object
const char *channelName;

/// A usable psk - which has been constructed based on the (possibly short psk) in channelSettings
static uint8_t activePSK[32];
static uint8_t activePSKSize;

/**
 * Generate a short suffix used to disambiguate channels that might have the same "name" entered by the human but different PSKs.
 * The ideas is that the PSK changing should be visible to the user so that they see they probably messed up and that's why they
their nodes
 * aren't talking to each other.
 *
 * This string is of the form "#name-X".
 *
 * Where X is either:
 * (for custom PSKS) a letter from A to Z (base26), and formed by xoring all the bytes of the PSK together,
 * OR (for the standard minimially secure PSKs) a number from 0 to 9.
 *
 * This function will also need to be implemented in GUI apps that talk to the radio.
 *
 * https://github.com/meshtastic/Meshtastic-device/issues/269
 */
const char *getChannelName()
{
    static char buf[32];

    char suffix;
    if (channelSettings.psk.size != 1) {
        // We have a standard PSK, so generate a letter based hash.
        uint8_t code = 0;
        for (int i = 0; i < activePSKSize; i++)
            code ^= activePSK[i];

        suffix = 'A' + (code % 26);
    } else {
        suffix = '0' + channelSettings.psk.bytes[0];
    }

    snprintf(buf, sizeof(buf), "#%s-%c", channelName, suffix);
    return buf;
}

NodeDB::NodeDB() : nodes(devicestate.node_db), numNodes(&devicestate.node_db_count) {}

bool NodeDB::resetRadioConfig()
{
    bool didFactoryReset = false;

    radioGeneration++;

    /// 16 bytes of random PSK for our _public_ default channel that all devices power up on (AES128)
    static const uint8_t defaultpsk[] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                         0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0xbf};

    if (radioConfig.preferences.factory_reset) {
        DEBUG_MSG("Performing factory reset!\n");
        installDefaultDeviceState();
        didFactoryReset = true;
    } else if (!channelSettings.psk.size) {
        DEBUG_MSG("Setting default preferences!\n");

        radioConfig.has_channel_settings = true;
        radioConfig.has_preferences = true;

        // radioConfig.modem_config = RadioConfig_ModemConfig_Bw125Cr45Sf128;  // medium range and fast
        // channelSettings.modem_config = ChannelSettings_ModemConfig_Bw500Cr45Sf128;  // short range and fast, but wide
        // bandwidth so incompatible radios can talk together
        channelSettings.modem_config = ChannelSettings_ModemConfig_Bw125Cr48Sf4096; // slow and long range

        channelSettings.tx_power = 0; // default
        uint8_t defaultpskIndex = 1;
        channelSettings.psk.bytes[0] = defaultpskIndex;
        channelSettings.psk.size = 1;
        strcpy(channelSettings.name, "");
    }

    // Convert the old string "Default" to our new short representation
    if (strcmp(channelSettings.name, "Default") == 0)
        *channelSettings.name = '\0';

    // Convert the short "" representation for Default into a usable string
    channelName = channelSettings.name;
    if (!*channelName) { // emptystring
        // Per mesh.proto spec, if bandwidth is specified we must ignore modemConfig enum, we assume that in that case
        // the app fucked up and forgot to set channelSettings.name

        if (channelSettings.bandwidth != 0)
            channelName = "Unset";
        else
            switch (channelSettings.modem_config) {
            case ChannelSettings_ModemConfig_Bw125Cr45Sf128:
                channelName = "Medium";
                break;
            case ChannelSettings_ModemConfig_Bw500Cr45Sf128:
                channelName = "ShortFast";
                break;
            case ChannelSettings_ModemConfig_Bw31_25Cr48Sf512:
                channelName = "LongAlt";
                break;
            case ChannelSettings_ModemConfig_Bw125Cr48Sf4096:
                channelName = "LongSlow";
                break;
            default:
                channelName = "Invalid";
                break;
            }
    }

    // Convert any old usage of the defaultpsk into our new short representation.
    if (channelSettings.psk.size == sizeof(defaultpsk) &&
        memcmp(channelSettings.psk.bytes, defaultpsk, sizeof(defaultpsk)) == 0) {
        *channelSettings.psk.bytes = 1;
        channelSettings.psk.size = 1;
    }

    // Convert the short single byte variants of psk into variant that can be used more generally
    memcpy(activePSK, channelSettings.psk.bytes, channelSettings.psk.size);
    activePSKSize = channelSettings.psk.size;
    if (activePSKSize == 1) {
        uint8_t pskIndex = activePSK[0];
        DEBUG_MSG("Expanding short PSK #%d\n", pskIndex);
        if (pskIndex == 0)
            activePSKSize = 0; // Turn off encryption
        else {
            memcpy(activePSK, defaultpsk, sizeof(defaultpsk));
            activePSKSize = sizeof(defaultpsk);
            // Bump up the last byte of PSK as needed
            uint8_t *last = activePSK + sizeof(defaultpsk) - 1;
            *last = *last + pskIndex - 1; // index of 1 means no change vs defaultPSK
        }
    }

    // Tell our crypto engine about the psk
    crypto->setKey(activePSKSize, activePSK);

    // temp hack for quicker testing
    // devicestate.no_save = true;
    if (devicestate.no_save) {
        DEBUG_MSG("***** DEVELOPMENT MODE - DO NOT RELEASE *****\n");

        // Sleep quite frequently to stress test the BLE comms, broadcast position every 6 mins
        radioConfig.preferences.screen_on_secs = 10;
        radioConfig.preferences.wait_bluetooth_secs = 10;
        radioConfig.preferences.position_broadcast_secs = 6 * 60;
        radioConfig.preferences.ls_secs = 60;
        radioConfig.preferences.region = RegionCode_TW;

        // Enter super deep sleep soon and stay there not very long
        // radioConfig.preferences.mesh_sds_timeout_secs = 10;
        // radioConfig.preferences.sds_secs = 60;
    }

    // Update the global myRegion
    initRegion();

    return didFactoryReset;
}

void NodeDB::installDefaultDeviceState()
{
    // We try to preserve the region setting because it will really bum users out if we discard it
    String oldRegion = myNodeInfo.region;
    RegionCode oldRegionCode = radioConfig.preferences.region;

    memset(&devicestate, 0, sizeof(devicestate));

    *numNodes = 0; // Forget node DB

    // init our devicestate with valid flags so protobuf writing/reading will work
    devicestate.has_my_node = true;
    devicestate.has_radio = true;
    devicestate.has_owner = true;
    devicestate.radio.has_channel_settings = true;
    devicestate.radio.has_preferences = true;
    devicestate.node_db_count = 0;
    devicestate.receive_queue_count = 0; // Not yet implemented FIXME

    resetRadioConfig();

    // default to no GPS, until one has been found by probing
    myNodeInfo.has_gps = false;
    myNodeInfo.message_timeout_msec = FLOOD_EXPIRE_TIME;
    generatePacketId(); // FIXME - ugly way to init current_packet_id;

    // Init our blank owner info to reasonable defaults
    getMacAddr(ourMacAddr);
    sprintf(owner.id, "!%02x%02x%02x%02x%02x%02x", ourMacAddr[0], ourMacAddr[1], ourMacAddr[2], ourMacAddr[3], ourMacAddr[4],
            ourMacAddr[5]);
    memcpy(owner.macaddr, ourMacAddr, sizeof(owner.macaddr));

    // Set default owner name
    pickNewNodeNum(); // Note: we will repick later, just in case the settings are corrupted, but we need a valid
    // owner.short_name now
    sprintf(owner.long_name, "Unknown %02x%02x", ourMacAddr[4], ourMacAddr[5]);
    sprintf(owner.short_name, "?%02X", (unsigned)(myNodeInfo.my_node_num & 0xff));

    // Restore region if possible
    if (oldRegionCode != RegionCode_Unset)
        radioConfig.preferences.region = oldRegionCode;
    if (oldRegion.length())
        strcpy(myNodeInfo.region, oldRegion.c_str());
}

void NodeDB::init()
{
    installDefaultDeviceState();

    // saveToDisk();
    loadFromDisk();
    // saveToDisk();

    // We set node_num and packet_id _after_ loading from disk, because we always want to use the values this
    // rom was compiled for, not what happens to be in the save file.
    myNodeInfo.node_num_bits = sizeof(NodeNum) * 8;
    myNodeInfo.packet_id_bits = sizeof(PacketId) * 8;

    myNodeInfo.error_code =
        CriticalErrorCode_None; // For the error code, only show values from this boot (discard value from flash)
    myNodeInfo.error_address = 0;

    // likewise - we always want the app requirements to come from the running appload
    myNodeInfo.min_app_version = 20120; // format is Mmmss (where M is 1+the numeric major number. i.e. 20120 means 1.1.20

    // Note! We do this after loading saved settings, so that if somehow an invalid nodenum was stored in preferences we won't
    // keep using that nodenum forever. Crummy guess at our nodenum (but we will check against the nodedb to avoid conflicts)
    pickNewNodeNum();

    // Include our owner in the node db under our nodenum
    NodeInfo *info = getOrCreateNode(getNodeNum());
    info->user = owner;
    info->has_user = true;

    // We set these _after_ loading from disk - because they come from the build and are more trusted than
    // what is stored in flash
    if (xstr(HW_VERSION)[0])
        strncpy(myNodeInfo.region, optstr(HW_VERSION), sizeof(myNodeInfo.region));
    else
        DEBUG_MSG("This build does not specify a HW_VERSION\n"); // Eventually new builds will no longer include this build flag

    // Check for the old style of region code strings, if found, convert to the new enum.
    // Those strings will look like "1.0-EU433"
    if (radioConfig.preferences.region == RegionCode_Unset && strncmp(myNodeInfo.region, "1.0-", 4) == 0) {
        const char *regionStr = myNodeInfo.region + 4; // EU433 or whatever
        for (const RegionInfo *r = regions; r->code != RegionCode_Unset; r++)
            if (strcmp(r->name, regionStr) == 0) {
                radioConfig.preferences.region = r->code;
                break;
            }
    }

    strncpy(myNodeInfo.firmware_version, optstr(APP_VERSION), sizeof(myNodeInfo.firmware_version));
    strncpy(myNodeInfo.hw_model, HW_VENDOR, sizeof(myNodeInfo.hw_model));

    resetRadioConfig(); // If bogus settings got saved, then fix them

    DEBUG_MSG("legacy_region=%s, region=%d, NODENUM=0x%x, dbsize=%d\n", myNodeInfo.region, radioConfig.preferences.region,
              myNodeInfo.my_node_num, *numNodes);
}

// We reserve a few nodenums for future use
#define NUM_RESERVED 4

/**
 * get our starting (provisional) nodenum from flash.
 */
void NodeDB::pickNewNodeNum()
{
    NodeNum r = myNodeInfo.my_node_num;

    // If we don't have a nodenum at app - pick an initial nodenum based on the macaddr
    if (r == 0)
        r = (ourMacAddr[2] << 24) | (ourMacAddr[3] << 16) | (ourMacAddr[4] << 8) | ourMacAddr[5];

    if (r == NODENUM_BROADCAST || r < NUM_RESERVED)
        r = NUM_RESERVED; // don't pick a reserved node number

    NodeInfo *found;
    while ((found = getNode(r)) && memcmp(found->user.macaddr, owner.macaddr, sizeof(owner.macaddr))) {
        NodeNum n = random(NUM_RESERVED, NODENUM_BROADCAST); // try a new random choice
        DEBUG_MSG("NOTE! Our desired nodenum 0x%x is in use, so trying for 0x%x\n", r, n);
        r = n;
    }

    myNodeInfo.my_node_num = r;
}

const char *preffile = "/db.proto";
const char *preftmp = "/db.proto.tmp";

void NodeDB::loadFromDisk()
{
#ifdef FS
    // static DeviceState scratch; We no longer read into a tempbuf because this structure is 15KB of valuable RAM

    auto f = FS.open(preffile);
    if (f) {
        DEBUG_MSG("Loading saved preferences\n");
        pb_istream_t stream = {&readcb, &f, DeviceState_size};

        // DEBUG_MSG("Preload channel name=%s\n", channelSettings.name);

        memset(&devicestate, 0, sizeof(devicestate));
        if (!pb_decode(&stream, DeviceState_fields, &devicestate)) {
            DEBUG_MSG("Error: can't decode protobuf %s\n", PB_GET_ERROR(&stream));
            installDefaultDeviceState(); // Our in RAM copy might now be corrupt
            // FIXME - report failure to phone
        } else {
            if (devicestate.version < DEVICESTATE_MIN_VER) {
                DEBUG_MSG("Warn: devicestate is old, discarding\n");
                installDefaultDeviceState();
            } else {
                DEBUG_MSG("Loaded saved preferences version %d\n", devicestate.version);
            }

            // DEBUG_MSG("Postload channel name=%s\n", channelSettings.name);
        }

        f.close();
    } else {
        DEBUG_MSG("No saved preferences found\n");
    }

#else
    DEBUG_MSG("ERROR: Filesystem not implemented\n");
#endif
}

void NodeDB::saveToDisk()
{
#ifdef FS
    if (!devicestate.no_save) {
        auto f = FS.open(preftmp, FILE_O_WRITE);
        if (f) {
            DEBUG_MSG("Writing preferences\n");

            pb_ostream_t stream = {&writecb, &f, SIZE_MAX, 0};

            // DEBUG_MSG("Presave channel name=%s\n", channelSettings.name);

            devicestate.version = DEVICESTATE_CUR_VER;
            if (!pb_encode(&stream, DeviceState_fields, &devicestate)) {
                DEBUG_MSG("Error: can't write protobuf %s\n", PB_GET_ERROR(&stream));
                // FIXME - report failure to phone

                f.close();
            } else {
                // Success - replace the old file
                f.close();

                // brief window of risk here ;-)
                if (!FS.remove(preffile))
                    DEBUG_MSG("Warning: Can't remove old pref file\n");
                if (!FS.rename(preftmp, preffile))
                    DEBUG_MSG("Error: can't rename new pref file\n");
            }
        } else {
            DEBUG_MSG("ERROR: can't write prefs\n"); // FIXME report to app
        }
    } else {
        DEBUG_MSG("***** DEVELOPMENT MODE - DO NOT RELEASE - not saving to flash *****\n");
    }
#else
    DEBUG_MSG("ERROR filesystem not implemented\n");
#endif
}

const NodeInfo *NodeDB::readNextInfo()
{
    if (readPointer < *numNodes)
        return &nodes[readPointer++];
    else
        return NULL;
}

/// Given a node, return how many seconds in the past (vs now) that we last heard from it
uint32_t sinceLastSeen(const NodeInfo *n)
{
    uint32_t now = getTime();

    uint32_t last_seen = n->position.time;
    int delta = (int)(now - last_seen);
    if (delta < 0) // our clock must be slightly off still - not set from GPS yet
        delta = 0;

    return delta;
}

#define NUM_ONLINE_SECS (60 * 2) // 2 hrs to consider someone offline

size_t NodeDB::getNumOnlineNodes()
{
    size_t numseen = 0;

    // FIXME this implementation is kinda expensive
    for (int i = 0; i < *numNodes; i++)
        if (sinceLastSeen(&nodes[i]) < NUM_ONLINE_SECS)
            numseen++;

    return numseen;
}

#include "MeshPlugin.h"

/** Update position info for this node based on received position data
 */
void NodeDB::updatePosition(uint32_t nodeId, const Position &p)
{
    NodeInfo *info = getOrCreateNode(nodeId);

    DEBUG_MSG("DB update position node=0x%x time=%u, latI=%d, lonI=%d\n", nodeId, p.time, p.latitude_i, p.longitude_i);

    info->position = p;
    info->has_position = true;
    updateGUIforNode = info;
    notifyObservers(true); // Force an update whether or not our node counts have changed
}

/** Update user info for this node based on received user data
 */
void NodeDB::updateUser(uint32_t nodeId, const User &p)
{
    NodeInfo *info = getOrCreateNode(nodeId);

    DEBUG_MSG("old user %s/%s/%s\n", info->user.id, info->user.long_name, info->user.short_name);

    bool changed = memcmp(&info->user, &p,
                          sizeof(info->user)); // Both of these blocks start as filled with zero so I think this is okay

    info->user = p;
    DEBUG_MSG("updating changed=%d user %s/%s/%s\n", changed, info->user.id, info->user.long_name, info->user.short_name);
    info->has_user = true;

    if (changed) {
        updateGUIforNode = info;
        powerFSM.trigger(EVENT_NODEDB_UPDATED);
        notifyObservers(true); // Force an update whether or not our node counts have changed

        // Not really needed - we will save anyways when we go to sleep
        // We just changed something important about the user, store our DB
        // saveToDisk();
    }
}

/// given a subpacket sniffed from the network, update our DB state
/// we updateGUI and updateGUIforNode if we think our this change is big enough for a redraw
void NodeDB::updateFrom(const MeshPacket &mp)
{
    if (mp.which_payload == MeshPacket_decoded_tag) {
        const SubPacket &p = mp.decoded;
        DEBUG_MSG("Update DB node 0x%x, rx_time=%u\n", mp.from, mp.rx_time);

        NodeInfo *info = getOrCreateNode(mp.from);

        if (mp.rx_time) {              // if the packet has a valid timestamp use it to update our last_seen
            info->has_position = true; // at least the time is valid
            info->position.time = mp.rx_time;
        }

        info->snr = mp.rx_snr; // keep the most recent SNR we received for this node.

        switch (p.which_payload) {
        case SubPacket_position_tag: {
            // handle a legacy position packet
            DEBUG_MSG("WARNING: Processing a (deprecated) position packet from %d\n", mp.from);
            updatePosition(mp.from, p.position);
            break;
        }

        case SubPacket_data_tag: {
            if (mp.to == NODENUM_BROADCAST || mp.to == nodeDB.getNodeNum())
                MeshPlugin::callPlugins(mp);
            break;
        }

        case SubPacket_user_tag: {
            DEBUG_MSG("WARNING: Processing a (deprecated) user packet from %d\n", mp.from);
            updateUser(mp.from, p.user);
            break;
        }

        default: {
            notifyObservers(); // If the node counts have changed, notify observers
        }
        }
    }
}

/// Find a node in our DB, return null for missing
/// NOTE: This function might be called from an ISR
NodeInfo *NodeDB::getNode(NodeNum n)
{
    for (int i = 0; i < *numNodes; i++)
        if (nodes[i].num == n)
            return &nodes[i];

    return NULL;
}

/// Find a node in our DB, create an empty NodeInfo if missing
NodeInfo *NodeDB::getOrCreateNode(NodeNum n)
{
    NodeInfo *info = getNode(n);

    if (!info) {
        // add the node
        assert(*numNodes < MAX_NUM_NODES);
        info = &nodes[(*numNodes)++];

        // everything is missing except the nodenum
        memset(info, 0, sizeof(*info));
        info->num = n;
    }

    return info;
}

/// Record an error that should be reported via analytics
void recordCriticalError(CriticalErrorCode code, uint32_t address)
{
    DEBUG_MSG("NOTE! Recording critical error %d, address=%x\n", code, address);
    myNodeInfo.error_code = code;
    myNodeInfo.error_address = address;
    myNodeInfo.error_count++;
}
