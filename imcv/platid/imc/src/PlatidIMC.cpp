/* 
 * Copyright (C) 2006-2011 Fachhochschule Hannover
 * (University of Applied Sciences and Arts, Hannover)
 * Faculty IV, Dept. of Computer Science
 * Ricklinger Stadtweg 118, 30459 Hannover, Germany
 * 
 * Email: trust@f4-i.fh-hannover.de
 * Website: http://trust.inform.fh-hannover.de/
 * 
 * This file is part of tnc@fhh, an open source 
 * Trusted Network Connect implementation by the Trust@FHH
 * research group at the Fachhochschule Hannover.
 * 
 * tnc@fhh is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * tnc@fhh is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with tnc@fhh; if not, see <http://www.gnu.org/licenses/>
 */
 
#include "PlatidIMC.h"

/* log4cxx includes */
#include <log4cxx/logger.h>
using namespace log4cxx;

/* openssl includes */
#include <openssl/rsa.h>
#include <openssl/engine.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/conf.h>

/* used for file reading */
#include <fstream>


/* defines for the configuration, generated by cmake */
#include "platidimcConfig.h"


static LoggerPtr
logger(Logger::getLogger("IMUnit.AbstractIMUnit.AbstractIMC.PlatidIMC"));


PlatidIMC::PlatidIMC(TNC_ConnectionID conID, PlatidIMCLibrary *imclib)
	: AbstractIMC(conID, imclib)
	, engine(NULL)
	, rsa(NULL)
	, certificate()
	, initialized(false)
	, useTpm(false)
	, useWksSrk(false)
	, certificateFile()
	, privateKeyFile()
{

	if (loadConfigFile() < 0)
		return;

	if (initializeEngine() < 0)
		return;

	if (loadRsa() < 0)
		return;

	if (loadCertificate() < 0)
		return;

	LOG4CXX_INFO(logger, "Successfully initialized!");
	initialized = true;
}



PlatidIMC::~PlatidIMC()
{
	LOG4CXX_TRACE(logger, "Destructor");
	if (engine != NULL) {
		LOG4CXX_TRACE(logger, "Finsihing and freeing engine structure...");
		ENGINE_finish(engine);
		ENGINE_free(engine);
		engine = NULL;
	}
	
	LOG4CXX_TRACE(logger, "Engine cleanup...");
	ENGINE_cleanup();

	if (rsa != NULL) {
		LOG4CXX_TRACE(logger, "Freeing rsa structure...");
		RSA_free(rsa);
		rsa = NULL;
	}
}



int PlatidIMC::loadConfigFile(void)
{
	LOG4CXX_TRACE(logger, "loadConfigFile()");
	ifstream cfgfile(IMC_CONFIG);
	string line;

	if (cfgfile.is_open()) {
		while (!cfgfile.eof()) {
			getline(cfgfile, line);
			processConfigLine(line);
		}
		cfgfile.close();
	} else {
		LOG4CXX_FATAL(logger, "Could not open " << IMC_CONFIG);
		return -1;
	}

	LOG4CXX_TRACE(logger, "Private key file = " << privateKeyFile);
	LOG4CXX_TRACE(logger, "Certificate file = " << certificateFile);
	if (privateKeyFile.length() == 0 || certificateFile.length() == 0) {
		LOG4CXX_FATAL(logger, "Failed parsing config file!");
		return -1;
	}
	return 0;
}



int PlatidIMC::processConfigLine(string configLine)
{
	LOG4CXX_TRACE(logger, "processConfigLine()");

	if (!(configLine.length() > 0)) {
		LOG4CXX_DEBUG(logger, "Found empty line");

	} else if (configLine.at(0) == '#') {
		LOG4CXX_DEBUG(logger, "Found comment line");

	} else  if (!configLine.compare(0, strlen(PLATIDIMC_CONFIG_USE_WKS),
			PLATIDIMC_CONFIG_USE_WKS)) {
		LOG4CXX_DEBUG(logger, "process use_wks_entry");
		processUseWksLine(configLine);

	} else if (!configLine.compare(0, strlen(PLATIDIMC_CONFIG_KEY_FILE),
			PLATIDIMC_CONFIG_KEY_FILE)) {
		LOG4CXX_DEBUG(logger, "process private key entry");
		processPrivateKeyLine(configLine);

	} else if (!configLine.compare(0, strlen(PLATIDIMC_CONFIG_CERTIFICATE_FILE),
			PLATIDIMC_CONFIG_CERTIFICATE_FILE)) {
		LOG4CXX_DEBUG(logger, "process certificate entry");
		processCertificateFileLine(configLine);
	} else {
		LOG4CXX_WARN(logger, "Found unknown entry in config");
	}
	return 0;
}



void PlatidIMC::processUseWksLine(string line)
{
	unsigned int i;
	if ((i = line.find_first_of(' ')) != std::string::npos) {
		if (!line.compare(i+1, strlen("yes"), "yes")) {
			LOG4CXX_DEBUG(logger, "Config file says we use the WKS");
			useWksSrk = true;
		}
	}
}



void PlatidIMC::processPrivateKeyLine(string line)
{
	unsigned int i;
	if ((i = line.find_first_of(' ')) != std::string::npos) {
		privateKeyFile = line.substr(i+1, line.length() - (i + 1));
		LOG4CXX_INFO(logger, "Private Key File = " << privateKeyFile);
	}
}



void PlatidIMC::processCertificateFileLine(string line)
{
	unsigned int i;
	if ((i = line.find_first_of(' ')) != std::string::npos) {
		certificateFile = line.substr(i + 1, line.length() - (i + 1));
		LOG4CXX_INFO(logger, "Certificate File = " << certificateFile);
	}
}

int PlatidIMC::initializeEngine(void)
{
	int res;
	LOG4CXX_TRACE(logger, "initializeEngine()");
	
	LOG4CXX_TRACE(logger, "ENGINE_load_dynamic()");
	ENGINE_load_dynamic();

	LOG4CXX_TRACE(logger, "Try to get the openssl_tpm_engine");
	engine = ENGINE_by_id(PLATIDIMC_TPM_ENGINE);
	
	if (engine == NULL) {
		LOG4CXX_WARN(logger, "Could not load TPM engine");
		useTpm = false;
	} else {
		LOG4CXX_INFO(logger, "Trying to initialize the TPM engine");
		if (ENGINE_init(engine)) {
			LOG4CXX_WARN(logger, "Initialized TPM engine.");
			useTpm = true;
		} else {
			LOG4CXX_WARN(logger, "Failed to initialize TPM engine!");
			//ENGINE_finish(engine);
			ENGINE_free(engine);
			engine = NULL;
			useTpm = false;
		}
	}

	if (!useTpm) {
		LOG4CXX_INFO(logger, "Using software mode...");
		LOG4CXX_TRACE(logger, "Calling ENGINE_cleanup()");
		ENGINE_cleanup();
		res = 1;
	} else {
		LOG4CXX_TRACE(logger, "Setting default RSA to loaded engine");

		if (!ENGINE_set_default_RSA(engine)) {
			LOG4CXX_FATAL(logger, "Could not set default RSA method");
			res = -1;
		} else {
			if (useWksSrk) {
				LOG4CXX_TRACE(logger, "Setting srk secret to"
							" well known secret");
				ENGINE_ctrl(engine, TPM_CMD_PIN, 1, NULL, NULL);
			}
			res = 0;
		}
	}
	return res;
}



int PlatidIMC::loadCertificate(void)
{
	LOG4CXX_TRACE(logger, "loadCertificate()");
	string line;
	LOG4CXX_INFO(logger, "Open Certificate: " << certificateFile);
	ifstream certfile(certificateFile.c_str());
	if (certfile.is_open()) {
		while (!certfile.eof()) {
			getline(certfile, line);
			certificate.append(line);
			certificate.append("\n");
		}
		certificate = certificate.substr(0, certificate.length() - 2);
	} else {
		LOG4CXX_ERROR(logger, "Could not open certificate!");
		return -1;
	}
	LOG4CXX_INFO(logger, "Loaded certificate:\n" << certificate.c_str());
	return 0;
}



EVP_PKEY * PlatidIMC::tpmLoadEvpPkey()
{
	LOG4CXX_TRACE(logger, "tpmLoadEvpPkey()");
	EVP_PKEY *evpkey;
	LOG4CXX_INFO(logger, "Loading private key from: " << privateKeyFile);
	evpkey = ENGINE_load_private_key(engine,
				privateKeyFile.c_str(), NULL, NULL);
	if (!evpkey) {
		LOG4CXX_ERROR(logger, "Could not load private key");
	}
	return evpkey;


}



EVP_PKEY * PlatidIMC::swLoadEvpPkey()
{
	BIO *in;
	LOG4CXX_TRACE(logger, "swLoadEvpPkey()");
	EVP_PKEY *evpkey;

	/* load all algorithms, needed for triple des pks */
	OpenSSL_add_all_algorithms();

	/* Used to lookup errors */
	ERR_load_CRYPTO_strings();
	SSL_load_error_strings();

	LOG4CXX_TRACE(logger, "Opening keyfile " << privateKeyFile );
	in = BIO_new_file(privateKeyFile.c_str(), "r");

	if (!in) {
		LOG4CXX_FATAL(logger, "Error opening private key");
	}

	LOG4CXX_TRACE(logger, "Loading keyfile...");

	/* cb = NULL and u = 0 => use default pwd cb method */
	evpkey = PEM_read_bio_PrivateKey(in, NULL, 0, 0);


	if (!evpkey) {
		LOG4CXX_FATAL(logger, "Could not read private key");
		char buffer[120];
		ERR_error_string(ERR_get_error(), buffer);
		LOG4CXX_FATAL(logger, "OpenSSL error: " << buffer);
	}

	BIO_free(in);

	return evpkey;

}



int PlatidIMC::loadRsa()
{
	int res = 0;
	LOG4CXX_TRACE(logger, "loadRsa()");
	EVP_PKEY *evpkey;

	if (useTpm) {
		evpkey = tpmLoadEvpPkey();
	} else {
		evpkey = swLoadEvpPkey();
	}

	if (!evpkey) {
		res = -1;
	} else {
		LOG4CXX_TRACE(logger, "Getting RSA key");
		rsa = EVP_PKEY_get1_RSA(evpkey);
		EVP_PKEY_free(evpkey);
	}

	if (!rsa) {
		LOG4CXX_ERROR(logger, "Could not get RSA key");
		res = -1;
	}
	return res;
}



/*
 * send our loaded certificate as first message.
 */
TNC_Result PlatidIMC::beginHandshake()
{
	if (initialized) {
		tncc.sendMessage((unsigned char *)certificate.c_str(),
				certificate.length() + 1,
				TNC_MESSAGETYPE_FHH_PLATID);
		return TNC_RESULT_SUCCESS;

	} else {
		LOG4CXX_FATAL(logger, "PlatidIMC not initialized. Won't start a handshake!");
		return TNC_RESULT_FATAL;
	}
}



TNC_Result PlatidIMC::receiveMessage(TNC_BufferReference message,
	                          TNC_UInt32 length,
	                          TNC_MessageType messageType)
{
	if (initialized) {
		int sig_length;
		LOG4CXX_INFO(logger, "Got message!");
		TNC_BufferReference dest = new TNC_Buffer[RSA_size(rsa)];

		sig_length = signNonce(message, length, dest);

		if (sig_length < 0) {
			LOG4CXX_FATAL(logger, "Encryption of nonce failed!");
			LOG4CXX_FATAL(logger, "Won't send it out!");
		} else {
			tncc.sendMessage(dest, sig_length,
					TNC_MESSAGETYPE_FHH_PLATID);
		}

		/* cleanup signature dest */
		delete[] dest;
	}
	return TNC_RESULT_SUCCESS;
}



/*
 * dest must be of size RSA_size(rsa)!
 */
int PlatidIMC::signNonce(TNC_BufferReference nonce, TNC_UInt32 length,
		TNC_BufferReference dest)
{
	int sig_length;
	LOG4CXX_TRACE(logger, "signNonce()");

	if (nonce == NULL || dest == NULL) {
		LOG4CXX_ERROR(logger, "nonce or dest is NULL!")
		sig_length = 0;
	} else {
		sig_length = RSA_private_encrypt(length, nonce, dest, rsa,
							RSA_PKCS1_PADDING);

	}
	LOG4CXX_INFO(logger, "Signature length: " << sig_length);

	return sig_length;
}



TNC_Result PlatidIMC::batchEnding()
{
	return TNC_RESULT_SUCCESS;
}



TNC_Result PlatidIMC::notifyConnectionChange()
{
	return TNC_RESULT_SUCCESS;
}
