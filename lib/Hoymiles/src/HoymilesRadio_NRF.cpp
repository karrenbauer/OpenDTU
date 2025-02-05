// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Thomas Basler and others
 */
#include "HoymilesRadio_NRF.h"
#include "Hoymiles.h"
#include "commands/RequestFrameCommand.h"
#include <Every.h>
#include <FunctionalInterrupt.h>

void HoymilesRadio_NRF::init(SPIClass* initialisedSpiBus, uint8_t pinCE, uint8_t pinIRQ)
{
    _dtuSerial.u64 = 0;

    _spiPtr.reset(initialisedSpiBus);
    _radio.reset(new RF24(pinCE, initialisedSpiBus->pinSS()));

    _radio->begin(_spiPtr.get());

    _radio->setDataRate(RF24_250KBPS);
    _radio->enableDynamicPayloads();
    _radio->setCRCLength(RF24_CRC_16);
    _radio->setAddressWidth(5);
    _radio->setRetries(0, 0);
    _radio->maskIRQ(true, true, false); // enable only receiving interrupts
    _isConfigured = true;
    if (!_radio->isChipConnected()) {
        Hoymiles.getMessageOutput()->println("NRF: Connection error!!");
        return;
    }
    Hoymiles.getMessageOutput()->println("NRF: Connection successful");

    attachInterrupt(digitalPinToInterrupt(pinIRQ), std::bind(&HoymilesRadio_NRF::handleIntr, this), FALLING);

    openReadingPipe();
    _radio->startListening();
    _isInitialized = true;
}

void HoymilesRadio_NRF::loop()
{
    if (!_isInitialized) {
        return;
    }

    EVERY_N_MILLIS(4)
    {
        switchRxCh();
    }

    if (_packetReceived) {
        Hoymiles.getMessageOutput()->println("Interrupt received");
        while (_radio->available()) {
            if (!(_rxBuffer.size() > FRAGMENT_BUFFER_SIZE)) {
                fragment_t f;
                memset(f.fragment, 0xcc, MAX_RF_PAYLOAD_SIZE);
                f.len = _radio->getDynamicPayloadSize();
                f.channel = _radio->getChannel();
                if (f.len > MAX_RF_PAYLOAD_SIZE)
                    f.len = MAX_RF_PAYLOAD_SIZE;
                _radio->read(f.fragment, f.len);
                _rxBuffer.push(f);
            } else {
                Hoymiles.getMessageOutput()->println("NRF: Buffer full");
                _radio->flush_rx();
            }
        }
        _packetReceived = false;

    } else {
        // Perform package parsing only if no packages are received
        if (!_rxBuffer.empty()) {
            fragment_t f = _rxBuffer.back();
            if (checkFragmentCrc(&f)) {
                std::shared_ptr<InverterAbstract> inv = Hoymiles.getInverterByFragment(&f);

                if (nullptr != inv) {
                    // Save packet in inverter rx buffer
                    Hoymiles.getMessageOutput()->printf("RX Channel: %d --> ", f.channel);
                    dumpBuf(f.fragment, f.len);
                    inv->addRxFragment(f.fragment, f.len);
                } else {
                    Hoymiles.getMessageOutput()->println("Inverter Not found!");
                }

            } else {
                Hoymiles.getMessageOutput()->println("Frame kaputt");
            }

            // Remove paket from buffer even it was corrupted
            _rxBuffer.pop();
        }
    }

    if (_busyFlag && _rxTimeout.occured()) {
        Hoymiles.getMessageOutput()->println("RX Period End");
        std::shared_ptr<InverterAbstract> inv = Hoymiles.getInverterBySerial(_commandQueue.front().get()->getTargetAddress());

        if (nullptr != inv) {
            CommandAbstract* cmd = _commandQueue.front().get();
            uint8_t verifyResult = inv->verifyAllFragments(cmd);
            if (verifyResult == FRAGMENT_ALL_MISSING_RESEND) {
                Hoymiles.getMessageOutput()->println("Nothing received, resend whole request");
                sendLastPacketAgain();

            } else if (verifyResult == FRAGMENT_ALL_MISSING_TIMEOUT) {
                Hoymiles.getMessageOutput()->println("Nothing received, resend count exeeded");
                _commandQueue.pop();
                _busyFlag = false;

            } else if (verifyResult == FRAGMENT_RETRANSMIT_TIMEOUT) {
                Hoymiles.getMessageOutput()->println("Retransmit timeout");
                _commandQueue.pop();
                _busyFlag = false;

            } else if (verifyResult == FRAGMENT_HANDLE_ERROR) {
                Hoymiles.getMessageOutput()->println("Packet handling error");
                _commandQueue.pop();
                _busyFlag = false;

            } else if (verifyResult > 0) {
                // Perform Retransmit
                Hoymiles.getMessageOutput()->print("Request retransmit: ");
                Hoymiles.getMessageOutput()->println(verifyResult);
                sendRetransmitPacket(verifyResult);

            } else {
                // Successful received all packages
                Hoymiles.getMessageOutput()->println("Success");
                _commandQueue.pop();
                _busyFlag = false;
            }
        } else {
            // If inverter was not found, assume the command is invalid
            Hoymiles.getMessageOutput()->println("RX: Invalid inverter found");
            _commandQueue.pop();
            _busyFlag = false;
        }
    } else if (!_busyFlag) {
        // Currently in idle mode --> send packet if one is in the queue
        if (!_commandQueue.empty()) {
            CommandAbstract* cmd = _commandQueue.front().get();

            auto inv = Hoymiles.getInverterBySerial(cmd->getTargetAddress());
            if (nullptr != inv) {
                inv->clearRxFragmentBuffer();
                sendEsbPacket(cmd);
            } else {
                Hoymiles.getMessageOutput()->println("TX: Invalid inverter found");
                _commandQueue.pop();
            }
        }
    }
}

void HoymilesRadio_NRF::setPALevel(rf24_pa_dbm_e paLevel)
{
    if (!_isInitialized) {
        return;
    }
    _radio->setPALevel(paLevel);
}

void HoymilesRadio_NRF::setDtuSerial(uint64_t serial)
{
    HoymilesRadio::setDtuSerial(serial);

    if (!_isInitialized) {
        return;
    }
    openReadingPipe();
}

bool HoymilesRadio_NRF::isConnected()
{
    if (!_isInitialized) {
        return false;
    }
    return _radio->isChipConnected();
}

bool HoymilesRadio_NRF::isPVariant()
{
    if (!_isInitialized) {
        return false;
    }
    return _radio->isPVariant();
}

void HoymilesRadio_NRF::openReadingPipe()
{
    serial_u s;
    s = convertSerialToRadioId(_dtuSerial);
    _radio->openReadingPipe(1, s.u64);
}

void HoymilesRadio_NRF::openWritingPipe(serial_u serial)
{
    serial_u s;
    s = convertSerialToRadioId(serial);
    _radio->openWritingPipe(s.u64);
}

void ARDUINO_ISR_ATTR HoymilesRadio_NRF::handleIntr()
{
    _packetReceived = true;
}

uint8_t HoymilesRadio_NRF::getRxNxtChannel()
{
    if (++_rxChIdx >= sizeof(_rxChLst))
        _rxChIdx = 0;
    return _rxChLst[_rxChIdx];
}

uint8_t HoymilesRadio_NRF::getTxNxtChannel()
{
    if (++_txChIdx >= sizeof(_txChLst))
        _txChIdx = 0;
    return _txChLst[_txChIdx];
}

void HoymilesRadio_NRF::switchRxCh()
{
    _radio->stopListening();
    _radio->setChannel(getRxNxtChannel());
    _radio->startListening();
}

void HoymilesRadio_NRF::sendEsbPacket(CommandAbstract* cmd)
{
    cmd->incrementSendCount();

    cmd->setRouterAddress(DtuSerial().u64);

    _radio->stopListening();
    _radio->setChannel(getTxNxtChannel());

    serial_u s;
    s.u64 = cmd->getTargetAddress();
    openWritingPipe(s);
    _radio->setRetries(3, 15);

    Hoymiles.getMessageOutput()->printf("TX %s Channel: %d --> ",
        cmd->getCommandName().c_str(), _radio->getChannel());
    cmd->dumpDataPayload(Hoymiles.getMessageOutput());
    _radio->write(cmd->getDataPayload(), cmd->getDataSize());

    _radio->setRetries(0, 0);
    openReadingPipe();
    _radio->setChannel(getRxNxtChannel());
    _radio->startListening();
    _busyFlag = true;
    _rxTimeout.set(cmd->getTimeout());
}
