// Copyright (c) 2017 Shift Devices AG
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dbb.h"

#include "crypto/aes.h"
#include "crypto/random.h"
#include "dbb_hid.h"
#include "support/cleanse.h"
#include "uint256.h"
#include "utilstrencodings.h"

#include "univalue.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <time.h>


const static int FIND_DEVICE_POLL_INTERVAL_IN_MS = 1000;
DBBDeviceManager::~DBBDeviceManager()
{
    DBB_DEBUG("Shutdown DBBDeviceManager\n");
    m_threadQueue.shutdown();
    m_stopCheckThread = true;
    m_stopExecuteThread = true;
    m_pollDeviceStateThread.join();
    m_commandQueueWorkThread.join();
    printf("stop...\n");
}

DBBDeviceManager::DBBDeviceManager(deviceStateChangedCallback stateChangeCallbackIn) : m_stopCheckThread(false), m_pauseCheckThread(false), m_stopExecuteThread(false), m_deviceChanged(stateChangeCallbackIn)
{
    DBB_DEBUG("Initialize HID communication interface\n");
    // for now, always use the HID communcation interface
    m_comInterface = std::unique_ptr<DBBCommunicationInterface>(new DBBCommunicationInterfaceHID());

    // dispatch the device state poll thread
    DBB_DEBUG("Start device state poll thread\n");
    m_pollDeviceStateThread = std::thread([this]() {
        DBBDeviceState currentDeviceState = DBBDeviceState::NoDevice;
        while (!m_stopCheckThread) {
            if (!m_pauseCheckThread) {
                DBBDeviceState state;
                std::string possibleDeviceIdentifier;
                {
                    std::lock_guard<std::mutex> lock(m_comLock);
                    DBB_DEBUG("   [Poll] poll device state\n");
                    state = m_comInterface->findDevice(possibleDeviceIdentifier);
                }
                if (currentDeviceState != state) {
                    // device state has changed, inform via callback
                    m_deviceChanged(state, possibleDeviceIdentifier);
                    currentDeviceState = state;
                }
            }
            DBB_DEBUG("   [Poll] start sleep\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(FIND_DEVICE_POLL_INTERVAL_IN_MS));
            DBB_DEBUG("   [Poll] end sleep\n");
        }
        DBB_DEBUG("   [Poll] exiting thread\n");
    });

    // dispatch the execution thread
    DBB_DEBUG("Start queue worker thread\n");
    m_commandQueueWorkThread = std::thread([this]() {
        // loop unless shutdown has been requested and queue is empty
        while (!m_stopExecuteThread || m_threadQueue.size() > 0) {
            // dequeue a execution package
            DBB_DEBUG("   [Execution] Start dequeued\n");
            commandPackage cmdCB = m_threadQueue.dequeue();
            if (m_threadQueue.isShutdown()) {
                break;
            }
            DBB_DEBUG("   [Execution] Command dequeued\n");
            std::string result;

            // open a connection, send command and close connection
            bool res = false;
            {
                std::lock_guard<std::mutex> lock(m_comLock);
                DBB_DEBUG("   [Execution] Open connection\n");
                if (m_comInterface->openConnection(std::string())) {
                    DBB_DEBUG("   [Execution] Send command\n");
                    res = m_comInterface->sendSynchronousJSON(cmdCB.first, result);
                    DBB_DEBUG("   [Execution] Close connection\n");
                    m_comInterface->closeConnection();
                }
            }

            // call callback with result
            DBB_DEBUG("   [Execution] Call callback\n");
            cmdCB.second(result, res ? 1 : 0);
        }
        DBB_DEBUG("   [Execution] exiting thread\n");
    });
}

void DBBDeviceManager::setStateChangeCallback(deviceStateChangedCallback stateChangeCallbackIn) {
    std::lock_guard<std::mutex> lock(m_comLock);
    m_deviceChanged = stateChangeCallbackIn;
}

bool DBBDeviceManager::decodeAndDecrypt(const std::string& base64Ciphertext, const std::string& passphrase, std::string& plaintextOut, bool hashPassphrase)
{
    if (base64Ciphertext.empty() || passphrase.empty())
        return false;

    std::string ciphertext = DecodeBase64(base64Ciphertext);
    uint256 passphraseHash;
    if (hashPassphrase) {
        Hash256().Write((unsigned char*)passphrase.data(), passphrase.size()).Finalize(passphraseHash.begin());
    }
    else {
        memcpy((unsigned char*)passphraseHash.begin(), &passphrase[0], std::min(static_cast<unsigned long>(passphraseHash.size()), sizeof(passphraseHash)));
    }

    plaintextOut.resize(ciphertext.size() - AES_BLOCKSIZE);

    AES256CBCDecrypt dec(passphraseHash.begin(), reinterpret_cast<const unsigned char*>(&ciphertext[0]) /* pass IV via cipertext buffer */, true);
    int size = dec.Decrypt(reinterpret_cast<const unsigned char*>(&ciphertext[0] + AES_BLOCKSIZE), ciphertext.size() - AES_BLOCKSIZE, reinterpret_cast<unsigned char*>(&plaintextOut[0]));
    plaintextOut.resize(size);

    memory_cleanse(passphraseHash.begin(), passphraseHash.size());

    return (size > 0);
}

bool DBBDeviceManager::encryptAndEncode(const std::string& json, const std::string& passphrase, std::string& encodeOut, bool hashPassphrase)
{
    if (passphrase.empty())
        return false;

    // KDF: use a double sha256 stretching
    // KDF strengt doesn't matter that much because the maximal
    // amount of attempts before the device gets erases is 15
    uint256 passphraseHash;
    if (hashPassphrase) {
        Hash256().Write((unsigned char*)passphrase.data(), passphrase.size()).Finalize(passphraseHash.begin());
    }
    else {
        memcpy((unsigned char*)passphraseHash.begin(), &passphrase[0], std::min(static_cast<unsigned long>(passphraseHash.size()), sizeof(passphraseHash)));
    }

    // create output buffer
    std::vector<unsigned char> ciphertext(json.size() + AES_BLOCKSIZE + AES_BLOCKSIZE); // ensure space for the IV

    // prefill the IV in front of the buffer
    GetRandBytes(&ciphertext[0], AES_BLOCKSIZE);

    // encrypt the json and write it to the buffer after the IV
    AES256CBCEncrypt enc(passphraseHash.begin(), &ciphertext[0] /* pass IV via cipertext buffer */, true);
    int size = enc.Encrypt(reinterpret_cast<const unsigned char*>(&json[0]), json.size(), &ciphertext[0] + AES_BLOCKSIZE);

    // resize the buffer, make sure we respect the IV space
    ciphertext.resize(size + AES_BLOCKSIZE);

    // base64 encoding
    encodeOut = EncodeBase64(&ciphertext[0], ciphertext.size());

    memory_cleanse(&ciphertext[0], ciphertext.size());
    memory_cleanse(passphraseHash.begin(), passphraseHash.size());

    return true;
}

bool DBBDeviceManager::decryptPossibleCiphertext(const std::string& originalSentCommand, const std::string& encryptedJSON, const std::string& passphrase, std::string& decryptedJsonOut)
{
    UniValue resultParsed;
    if (resultParsed.read(encryptedJSON)) {
        UniValue ctext = find_value(resultParsed, "ciphertext");
        if (ctext.isStr()) {
            // seems to be encrypted

            /* detect a possible passphrase change leading to the fact that
             * the decryption passphrase is different (new passphrase)
             */
            UniValue originalCommand;
            std::string passphraseToDecrypt = passphrase;
            if (resultParsed.read(originalSentCommand)) {
                UniValue password = find_value(resultParsed, "password");
                if (password.isStr()) {
                    DBB_DEBUG("   [Decryption] Found password command, switching decryption passphrase\n");
                    //we have change the passphrase, use the new one do decrypt
                    passphraseToDecrypt = password.get_str();
                }
            }
            if (decodeAndDecrypt(ctext.get_str(), passphraseToDecrypt, decryptedJsonOut)) {
                memory_cleanse(&passphraseToDecrypt[0], passphraseToDecrypt.size());
                return true;
            }
            else {
                memory_cleanse(&passphraseToDecrypt[0], passphraseToDecrypt.size());
                decryptedJsonOut.clear();
            }
        }
    }
    return false;
}

bool DBBDeviceManager::sendSynchronousCommand(const std::string& json, const std::string& passphrase, std::string& result, bool encrypt)
{
    DBB_DEBUG("Send synchronous command\n");
    std::string textToSend = json;
    if (encrypt) {
        encryptAndEncode(json, passphrase, textToSend);
    }
    bool res = false;
    {
        std::lock_guard<std::mutex> lock(m_comLock);
        DBB_DEBUG("[SYNC] open connection\n");
        if (m_comInterface->openConnection(std::string())) {
            DBB_DEBUG("[SYNC] send command\n");
            res = m_comInterface->sendSynchronousJSON(textToSend, result);
            DBB_DEBUG("[SYNC] close connection\n");
            m_comInterface->closeConnection();
        }
    }
    if (!res) {
        return false;
    }

    // try to decrypt
    std::string possibleDecryptedValue;
    if (decryptPossibleCiphertext(json, result, passphrase, possibleDecryptedValue)) {
        result = possibleDecryptedValue;
    }

    return true;
}

bool DBBDeviceManager::sendCommand(const std::string& json, const std::string& passphrase, commandCallback callback, bool encrypt)
{
    std::string textToSend = json;
    if (encrypt) {
        encryptAndEncode(json, passphrase, textToSend);
    }
    DBB_DEBUG("[sendCommand] Add command to the queue\n");
    m_threadQueue.enqueue(commandPackage(textToSend, [this, json, passphrase, callback](const std::string& result, int status) {
        // parse result and try to decrypt
        std::string valueToPass = result;
        std::string possibleDecryptedValue;
        if (decryptPossibleCiphertext(json, result, passphrase, possibleDecryptedValue)) {
            valueToPass = possibleDecryptedValue;
        }
        DBB_DEBUG("[sendCommand] Call callback\n");
        callback(valueToPass, status);
    }));
    return true;
}

bool DBBDeviceManager::upgradeFirmware(const std::string& filename, progressCallback progressCB)
{
    return upgradeFirmware(filename, std::move(progressCB), false, [](const std::vector<unsigned char>& firmwareBuffer){ return std::string(); });
}

bool DBBDeviceManager::upgradeFirmware(const std::string& filename, progressCallback progressCB, bool developmentDevice, std::function<std::string(const std::vector<unsigned char>& firmwareBuffer)> sigCreationCallback)
{
    // prepare the buffer
    std::vector<unsigned char> firmwareBuffer(FIRMWARE_SIGLEN+DBB_FIRMWARE_LENGTH);

    // load file
    bool res = false;
    std::ifstream firmwareFile(filename, std::ios::binary | std::ios::ate);
    std::streamsize firmwareSize = firmwareFile.tellg();
    if (firmwareSize <= 0 || firmwareSize > FIRMWARE_SIGLEN+DBB_FIRMWARE_LENGTH) {
        // file is empty, abort (or too large)
        return res;
    }

    // copy data into the buffer
    firmwareFile.seekg(0, std::ios::beg);
    unsigned int pos = 0;
    while (true) {
        firmwareFile.read(reinterpret_cast<char*>(&firmwareBuffer[0] + pos), FIRMWARE_CHUNKSIZE);
        std::streamsize bytes = firmwareFile.gcount();
        if (bytes == 0)
            break;

        pos += bytes;
    }
    firmwareFile.close();

    // continue upgrade with buffer
    return upgradeFirmware(firmwareBuffer, firmwareSize, progressCB, developmentDevice, std::move(sigCreationCallback));
}

bool DBBDeviceManager::upgradeFirmware(const std::vector<unsigned char> firmwareAndSigBuffer, const size_t firmwareSize, progressCallback progressCB, bool developmentDevice, std::function<std::string(const std::vector<unsigned char>& firmwareBuffer)> sigCreationCallback) {
    std::string sigStr;

    if (firmwareAndSigBuffer.size() < FIRMWARE_SIGLEN || firmwareAndSigBuffer.size() < firmwareSize) {
        return false;
    }

    int sigOffset = 0;
    //read signatures
    if (!developmentDevice) {
        unsigned char sigByte[FIRMWARE_SIGLEN];
        memcpy(&sigByte[0], &firmwareAndSigBuffer[0], FIRMWARE_SIGLEN);
        sigStr = HexStr(sigByte, sigByte + FIRMWARE_SIGLEN);
        sigOffset+=FIRMWARE_SIGLEN;
    }

    //read firmware
    std::vector<unsigned char> firmwareBuffer(DBB_FIRMWARE_LENGTH);
    assert(firmwareSize <= DBB_FIRMWARE_LENGTH);
    memcpy(&firmwareBuffer[0], &firmwareAndSigBuffer[sigOffset], firmwareSize-sigOffset);

    // append 0xff to the rest of the firmware buffer
    memset((void*)(&firmwareBuffer[firmwareSize-sigOffset]), 0xff, firmwareBuffer.size() - firmwareSize - sigOffset);

    // apply dummy signature
    if (developmentDevice) {
        // generate signature via callback
        // we don't want the whole ECDSA codebase in this library
        std::string possibleSigStr = sigCreationCallback(firmwareBuffer);
        if (possibleSigStr.size() >= 64) {
            sigStr = possibleSigStr;
        }
    }

    bool res = false;
    {
        std::lock_guard<std::mutex> lock(m_comLock);
        m_pauseCheckThread = true;
        res = m_comInterface->upgradeFirmware(firmwareBuffer, firmwareSize, sigStr, std::move(progressCB));
        m_pauseCheckThread = false;
    }
    return res;
}

DBBDeviceState DBBDeviceManager::findDevice(std::string& deviceIdentifierOut) {
    DBB_DEBUG("[findDevice] call findDevice\n");
    std::lock_guard<std::mutex> lock(m_comLock);
    return m_comInterface->findDevice(deviceIdentifierOut);
}
