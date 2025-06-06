#include "stdafx.h"
#include "InfluxPush.h"
#include "../hardware/hardwaretypes.h"
#include "../httpclient/HTTPClient.h"
#include <json/json.h>
#include "../main/Helper.h"
#include "../main/json_helper.h"
#include "../main/Logger.h"
#include "../main/mainworker.h"
#include "../main/RFXtrx.h"
#include "../main/SQLHelper.h"
#include "../main/WebServer.h"
#include "../webserver/Base64.h"
#include "../webserver/cWebem.h"
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

extern CInfluxPush m_influxpush;

CInfluxPush::CInfluxPush()
{
	m_PushType = PushType::PUSHTYPE_INFLUXDB;
	m_bLinkActive = false;
}

bool CInfluxPush::Start()
{
	Stop();

	RequestStart();

	UpdateSettings();
	ReloadPushLinks(m_PushType);

	m_thread = std::make_shared<std::thread>([this] { Do_Work(); });
	SetThreadName(m_thread->native_handle(), "InfluxPush");

	m_sConnection = m_mainworker.sOnDeviceReceived.connect([this](auto id, auto idx, const auto &name, auto rx) { OnDeviceReceived(id, idx, name, rx); });

	return (m_thread != nullptr);
}

void CInfluxPush::Stop()
{
	if (m_sConnection.connected())
		m_sConnection.disconnect();

	if (m_thread)
	{
		RequestStop();
		m_thread->join();
		m_thread.reset();
	}
}

void CInfluxPush::UpdateSettings()
{
	int fActive = 0;
	m_sql.GetPreferencesVar("InfluxActive", fActive);
	m_bLinkActive = (fActive == 1);

	fActive = 0;
	m_sql.GetPreferencesVar("InfluxVersion2", fActive);
	m_bInfluxVersion2 = (fActive == 1);

	m_InfluxPort = 8086;
	m_sql.GetPreferencesVar("InfluxIP", m_InfluxIP);
	m_sql.GetPreferencesVar("InfluxPort", m_InfluxPort);
	m_sql.GetPreferencesVar("InfluxPath", m_InfluxPath);
	m_sql.GetPreferencesVar("InfluxDatabase", m_InfluxDatabase);
	m_sql.GetPreferencesVar("InfluxUsername", m_InfluxUsername);
	m_sql.GetPreferencesVar("InfluxPassword", m_InfluxPassword);

	int InfluxDebugActiveInt = 0;
	m_bInfluxDebugActive = false;
	m_sql.GetPreferencesVar("InfluxDebug", InfluxDebugActiveInt);
	if (InfluxDebugActiveInt == 1)
	{
		m_bInfluxDebugActive = true;
	}
	m_szURL = "";
	if ((m_InfluxIP.empty()) || (m_InfluxPort == 0) || (m_InfluxDatabase.empty()))
		return;
	std::stringstream sURL;
	if (m_InfluxIP.find("://") == std::string::npos)
		sURL << "http://";
	sURL << m_InfluxIP << ":" << m_InfluxPort;
	if (!m_InfluxPath.empty())
		sURL << "/" << m_InfluxPath;

	if (m_bInfluxVersion2)
		sURL << "/api/v2" << m_InfluxPath;

	sURL << "/write?";

	if (!m_bInfluxVersion2)
	{
		if ((!m_InfluxUsername.empty()) && (!m_InfluxPassword.empty()))
			sURL << "u=" << m_InfluxUsername << "&p=" << base64_decode(m_InfluxPassword) << "&";
		sURL << "db=" << m_InfluxDatabase;
	}
	else
	{
		sURL << "org=" << m_InfluxUsername;
		sURL << "&bucket=" << m_InfluxDatabase;
	}
	sURL << "&precision=s";
	m_szURL = sURL.str();
}

void CInfluxPush::OnDeviceReceived(int m_HwdID, uint64_t DeviceRowIdx, const std::string &DeviceName, const unsigned char *pRXCommand)
{
	DoInfluxPush(DeviceRowIdx);
}

void CInfluxPush::DoInfluxPush(const uint64_t DeviceRowIdx, const bool bForced)
{
	if (!m_bLinkActive)
		return;
	if (!IsLinkInDatabase(DeviceRowIdx))
		return;

	std::vector<std::vector<std::string>> result;
	result = m_sql.safe_query(
		"SELECT A.DeviceRowID, A.DelimitedValue, B.ID, B.Type, B.SubType, B.nValue, B.sValue, A.TargetType, A.IncludeUnit, B.Name, B.SwitchType FROM PushLink as A, DeviceStatus as B "
		"WHERE (A.PushType==%d AND A.DeviceRowID == '%" PRIu64 "' AND A.Enabled==1 AND A.DeviceRowID==B.ID)",
		PushType::PUSHTYPE_INFLUXDB, DeviceRowIdx);
	if (result.empty())
		return;

	int dType = atoi(result[0][3].c_str());
	int dSubType = atoi(result[0][4].c_str());
	int nValue = atoi(result[0][5].c_str());
	std::string sValue = result[0][6];
	std::string name = result[0][9];
	int metertype = atoi(result[0][10].c_str());

	time_t atime = mytime(nullptr);
	for (const auto &sd : result)
	{
		std::string sendValue;
		int delpos = atoi(sd[1].c_str());
		int targetType = atoi(sd[7].c_str());
		int includeUnit = atoi(sd[8].c_str());

		std::vector<std::string> strarray;
		if (sValue.find(';') != std::string::npos)
		{
			std::string rawsendValue("");
			StringSplit(sValue, ";", strarray);
			if (int(strarray.size()) >= delpos)
			{
				rawsendValue = strarray[delpos - 1];
			}
			sendValue = ProcessSendValue(DeviceRowIdx, rawsendValue, delpos, nValue, sValue, includeUnit, dType, dSubType, metertype);
		}
		else
			sendValue = ProcessSendValue(DeviceRowIdx, sValue, delpos, nValue, sValue, includeUnit, dType, dSubType, metertype);

		if (sendValue.empty())
			continue;

		std::string szKey;
		std::string vType = CBasePush::DropdownOptionsValue(dType, dSubType, delpos);
		stdreplace(vType, " ", "-");
		stdreplace(name, " ", "-");
		szKey = vType + ",idx=" + sd[0] + ",name=" + name;

		_tPushItem pItem;
		pItem.skey = szKey;
		pItem.stimestamp = atime;
		pItem.svalue = sendValue;

		if ((targetType == 0) && (!bForced))
		{
			// Only send on change
			auto itt = m_PushedItems.find(szKey);
			if (itt != m_PushedItems.end())
			{
				if (sendValue == itt->second.svalue)
					continue;
			}
			m_PushedItems[szKey] = pItem;
		}

		std::lock_guard<std::mutex> l(m_background_task_mutex);
		if (m_background_task_queue.size() < 50)
			m_background_task_queue.push_back(pItem);
	}
}

void CInfluxPush::Do_Work()
{
	std::vector<_tPushItem> _items2do;

	while (!IsStopRequested(500))
	{
		{ // additional scope for lock (accessing size should be within lock too)
			std::lock_guard<std::mutex> l(m_background_task_mutex);
			if (m_background_task_queue.empty())
				continue;
			_items2do = m_background_task_queue;
			m_background_task_queue.clear();
		}

		if (m_szURL.empty())
			continue;

		std::string sSendData;

		for (const auto &item : _items2do)
		{
			if (!sSendData.empty())
				sSendData += '\n';

			std::stringstream sziData;
			sziData << item.skey << " value=";
			if (item.skey.find("Text,") == 0)
				sziData << "\"" << item.svalue << "\"";
			else
				sziData << item.svalue;
			if (m_bInfluxDebugActive)
			{
				_log.Log(LOG_NORM, "InfluxLink: value %s", sziData.str().c_str());
			}
			sziData << " " << item.stimestamp;
			sSendData += sziData.str();
		}
		std::vector<std::string> ExtraHeaders;
		std::string sResult;
		if (m_bInfluxVersion2)
		{
			ExtraHeaders.push_back("Authorization: Token " + base64_decode(m_InfluxPassword));
			ExtraHeaders.push_back("Content-type: text/plain");
		}

		bool bRet = HTTPClient::POST(m_szURL, sSendData, ExtraHeaders, sResult, true, true);
		if (!bRet)
		{
			_log.Log(LOG_ERROR, "InfluxLink: Error sending data to InfluxDB server! (check address/port/database/username/password)");
		}
		else if (!sResult.empty())
		{
			Json::Value root;
			bool ret = ParseJSon(sResult, root);
			if (root.isObject())
			{
				if (!root["code"].empty())
				{
					std::string szCode = root["code"].asString();
					bool bHaveError = false;
					std::string szMessage;

					if (
						(szCode == "unauthorized")
						|| (szCode == "forbidden")
						)
					{
						bHaveError = true;
						szMessage = root["message"].asString();
					}

					if (bHaveError)
					{
						_log.Log(LOG_ERROR, "InfluxLink: Error sending data to InfluxDB server! (%s)", szMessage.c_str());
					}
				}
			}
		}
	}
}

// Webserver helpers
namespace http
{
	namespace server
	{
		void CWebServer::Cmd_SaveInfluxLinkConfig(WebEmSession &session, const request &req, Json::Value &root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string linkactive = request::findValue(&req, "linkactive");
			std::string isversion2 = request::findValue(&req, "isversion2");
			std::string remote = request::findValue(&req, "remote");
			std::string port = request::findValue(&req, "port");
			std::string path = request::findValue(&req, "path");
			std::string database = request::findValue(&req, "database");
			std::string username = request::findValue(&req, "username");
			std::string password = request::findValue(&req, "password");
			std::string debugenabled = request::findValue(&req, "debugenabled");
			if ((linkactive.empty()) || (remote.empty()) || (port.empty()) || (database.empty()) || (debugenabled.empty()))
				return;
			int ilinkactive = atoi(linkactive.c_str());
			int idebugenabled = atoi(debugenabled.c_str());
			m_sql.UpdatePreferencesVar("InfluxActive", ilinkactive);
			m_sql.UpdatePreferencesVar("InfluxVersion2", atoi(isversion2.c_str()));
			m_sql.UpdatePreferencesVar("InfluxIP", remote);
			m_sql.UpdatePreferencesVar("InfluxPort", atoi(port.c_str()));
			m_sql.UpdatePreferencesVar("InfluxPath", path);
			m_sql.UpdatePreferencesVar("InfluxDatabase", database);
			m_sql.UpdatePreferencesVar("InfluxUsername", username);
			m_sql.UpdatePreferencesVar("InfluxPassword", base64_encode(password));
			m_sql.UpdatePreferencesVar("InfluxDebug", idebugenabled);
			m_influxpush.UpdateSettings();
			root["status"] = "OK";
			root["title"] = "SaveInfluxLinkConfig";
		}

		void CWebServer::Cmd_GetInfluxLinkConfig(WebEmSession &session, const request &req, Json::Value &root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string sValue;
			int nValue;
			if (m_sql.GetPreferencesVar("InfluxActive", nValue))
			{
				root["InfluxActive"] = nValue;
			}
			else
			{
				root["InfluxActive"] = 0;
			}
			if (m_sql.GetPreferencesVar("InfluxVersion2", nValue))
			{
				root["InfluxVersion2"] = nValue;
			}
			else
			{
				root["InfluxVersion2"] = 0;
			}
			if (m_sql.GetPreferencesVar("InfluxIP", sValue))
			{
				root["InfluxIP"] = sValue;
			}
			if (m_sql.GetPreferencesVar("InfluxPort", nValue))
			{
				root["InfluxPort"] = nValue;
			}
			if (m_sql.GetPreferencesVar("InfluxPath", sValue))
			{
				root["InfluxPath"] = sValue;
			}
			if (m_sql.GetPreferencesVar("InfluxDatabase", sValue))
			{
				root["InfluxDatabase"] = sValue;
			}
			if (m_sql.GetPreferencesVar("InfluxUsername", sValue))
			{
				root["InfluxUsername"] = sValue;
			}
			if (m_sql.GetPreferencesVar("InfluxPassword", sValue))
			{
				root["InfluxPassword"] = base64_decode(sValue);
			}
			if (m_sql.GetPreferencesVar("InfluxDebug", nValue))
			{
				root["InfluxDebug"] = nValue;
			}
			else
			{
				root["InfluxDebug"] = 0;
			}
			root["status"] = "OK";
			root["title"] = "GetInfluxLinkConfig";
		}

		void CWebServer::Cmd_GetInfluxLinks(WebEmSession &session, const request &req, Json::Value &root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::vector<std::vector<std::string>> result;
			result = m_sql.safe_query("SELECT A.ID,A.DeviceRowID,A.Delimitedvalue,A.TargetType,A.TargetVariable,A.TargetDeviceID,A.TargetProperty,A.Enabled, B.Name, A.IncludeUnit, "
						  "B.Type, B.SubType FROM PushLink as A, DeviceStatus as B WHERE (A.PushType==%d AND A.DeviceRowID==B.ID)",
						  CBasePush::PushType::PUSHTYPE_INFLUXDB);
			if (!result.empty())
			{
				int ii = 0;
				for (const auto &sd : result)
				{
					int Delimitedvalue = std::stoi(sd[2]);
					int devType = std::stoi(sd[10]);
					int devSubType = std::stoi(sd[11]);

					root["result"][ii]["idx"] = sd[0];
					root["result"][ii]["DeviceID"] = sd[1];
					root["result"][ii]["Delimitedvalue"] = Delimitedvalue;
					root["result"][ii]["Delimitedname"] = CBasePush::DropdownOptionsValue(devType, devSubType, Delimitedvalue);
					root["result"][ii]["TargetType"] = sd[3];
					root["result"][ii]["TargetVariable"] = sd[4];
					root["result"][ii]["TargetDevice"] = sd[5];
					root["result"][ii]["TargetProperty"] = sd[6];
					root["result"][ii]["Enabled"] = sd[7];
					root["result"][ii]["Name"] = sd[8];
					root["result"][ii]["IncludeUnit"] = sd[9];
					ii++;
				}
			}
			root["status"] = "OK";
			root["title"] = "GetInfluxLinks";
		}

		void CWebServer::Cmd_SaveInfluxLink(WebEmSession &session, const request &req, Json::Value &root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}
			std::string idx = request::findValue(&req, "idx");
			std::string deviceid = request::findValue(&req, "deviceid");
			int deviceidi = atoi(deviceid.c_str());
			std::string valuetosend = request::findValue(&req, "valuetosend");
			std::string targettype = request::findValue(&req, "targettype");
			int targettypei = atoi(targettype.c_str());
			std::string linkactive = request::findValue(&req, "linkactive");
			if (idx == "0")
			{
				//check if we already have this link
				auto result = m_sql.safe_query("SELECT ID FROM PushLink WHERE (PushType==%d AND DeviceRowID==%d AND DelimitedValue==%d AND TargetType==%d)",
					CBasePush::PushType::PUSHTYPE_INFLUXDB, deviceidi, atoi(valuetosend.c_str()), targettypei);
				if (!result.empty())
					return; //already have this
				m_sql.safe_query("INSERT INTO PushLink (PushType,DeviceRowID,DelimitedValue,TargetType,TargetVariable,TargetDeviceID,TargetProperty,IncludeUnit,Enabled) VALUES "
						 "(%d,'%d',%d,%d,'%q',%d,'%q',%d,%d)",
						 CBasePush::PushType::PUSHTYPE_INFLUXDB, deviceidi, atoi(valuetosend.c_str()), targettypei, "-", 0, "-", 0, atoi(linkactive.c_str()));
			}
			else
			{
				m_sql.safe_query("UPDATE PushLink SET DeviceRowID=%d, DelimitedValue=%d, TargetType=%d, Enabled=%d WHERE (ID == '%q')", deviceidi, atoi(valuetosend.c_str()),
						 targettypei, atoi(linkactive.c_str()), idx.c_str());
			}
			m_influxpush.ReloadPushLinks(CBasePush::PushType::PUSHTYPE_INFLUXDB);
			root["status"] = "OK";
			root["title"] = "SaveInfluxLink";
		}

		void CWebServer::Cmd_DeleteInfluxLink(WebEmSession &session, const request &req, Json::Value &root)
		{
			if (session.rights != 2)
			{
				session.reply_status = reply::forbidden;
				return; // Only admin user allowed
			}

			std::string idx = request::findValue(&req, "idx");
			if (idx.empty())
				return;
			m_sql.safe_query("DELETE FROM PushLink WHERE (ID=='%q')", idx.c_str());
			m_influxpush.ReloadPushLinks(CBasePush::PushType::PUSHTYPE_INFLUXDB);
			root["status"] = "OK";
			root["title"] = "DeleteInfluxLink";
		}
	} // namespace server
} // namespace http
