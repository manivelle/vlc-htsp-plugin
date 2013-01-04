/*****************************************************************************
 * Copyright (C) 2012
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#define __STDC_CONSTANT_MACROS 1

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_access.h>
#include <vlc_playlist.h>
#include <vlc_url.h>
#include <vlc_network.h>
#include <vlc_epg.h>
#include <vlc_meta.h>
#include <vlc_services_discovery.h>

#include <ctime>
#include <cerrno>

#include <list>
#include <deque>
#include <string>
#include <memory>
#include <sstream>
#include <unordered_map>

#include "htsmessage.h"
#include "sha1.h"

static int OpenHTSP(vlc_object_t *);
static void CloseHTSP(vlc_object_t *);
static int DemuxHTSP(demux_t *demux);
static int ControlHTSP(demux_t *access, int i_query, va_list args);

static int OpenSD(vlc_object_t *);
static void CloseSD(vlc_object_t *);
VLC_SD_PROBE_HELPER( "htsp", "Tvheadend HTSP", SD_CAT_LAN )

#define CFG_PREFIX "htsp-"

vlc_module_begin ()
	set_shortname( "HTSP Protocol" )
	set_description( "TVHeadend HTSP Protocol" )
	set_capability( "access_demux", 0 )
	set_category( CAT_INPUT )
	set_subcategory( SUBCAT_INPUT_ACCESS )
	set_callbacks( OpenHTSP, CloseHTSP )
	add_shortcut( "hts", "htsp" )

	add_submodule()
	set_shortname( "HTSP Protocol Discovery" )
	set_description( "TVHeadend HTSP Protocol Discovery" )
	set_category( CAT_PLAYLIST )
	set_subcategory ( SUBCAT_PLAYLIST_SD )
	add_integer_with_range( CFG_PREFIX"port", 9982, 1, 65536, "HTSP Server Port", "The port of the HTSP server to connect to", false )
	add_string( CFG_PREFIX"host", "localhost", "HTSP Server Address", "The IP/Hostname of the HTSP server to connect to", false )
	add_string( CFG_PREFIX"user", "", "HTSP Username", "The username for authentication with HTSP Server", false )
	add_string( CFG_PREFIX"pass", "", "HTSP Password", "The password for authentication with HTSP Server", false )
	set_capability ( "services_discovery", 0 )
	set_callbacks ( OpenSD, CloseSD )
	add_shortcut( "hts", "htsp" )

	VLC_SD_PROBE_SUBMODULE
vlc_module_end ()

static const char *const cfg_options[] =
{
	"port",
	"host",
	"user",
	"pass",
	NULL
};

struct hts_stream
{
	hts_stream()
		:es(0)
		,lastDts(0)
		,lastPts(0)
	{}

	es_out_id_t *es;
	es_format_t fmt;
	mtime_t lastDts;
	mtime_t lastPts;
};

struct demux_sys_t
{
	demux_sys_t()
		:start(0)
		,pcrStream(0)
		,lastPcr(0)
		,ptsDelay(300000)
		,netfd(-1)
		,streamCount(0)
		,stream(0)
		,host("")
		,port(0)
		,username("")
		,password("")
		,channelId(0)
		,nextSeqNum(1)
		,hadIFrame(false)
		,drops(0)
	{}

	~demux_sys_t()
	{
		if(stream)
			delete[] stream;

		if(netfd >= 0)
			net_Close(netfd);

		vlc_UrlClean(&url);
	}

	mtime_t start;
	uint32_t pcrStream;
	mtime_t lastPcr;
	mtime_t ptsDelay;

	int netfd;

	uint32_t streamCount;
	hts_stream *stream;

	vlc_url_t url;

	std::string host;
	uint16_t port;
	std::string username;
	std::string password;
	int channelId;

	std::string serverName;
	std::string serverVersion;
	int32_t protoVersion;

	uint32_t nextSeqNum;
	std::deque<HtsMessage> queue;

	bool hadIFrame;

	uint32_t drops;
};

struct tmp_channel
{
	std::string name;
	uint32_t cid;
	uint32_t cnum;
	std::string url;
	input_item_t *item;
};

struct services_discovery_sys_t
{
	services_discovery_sys_t()
		:netfd(-1)
		,nextSeqNum(1)
	{}

	int netfd;
	uint32_t nextSeqNum;
	std::deque<HtsMessage> queue;
	vlc_thread_t thread;
	std::unordered_map<uint32_t, tmp_channel> channelMap;
};

#define DEMUX_EOF 0
#define DEMUX_OK 1
#define DEMUX_ERROR -1

#define MAX_QUEUE_SIZE 1000
#define READ_TIMEOUT 10

/***************************************************
 ****       HTS Protocol Helper Functions       ****
 ***************************************************/

uint32_t HTSPNextSeqNum(demux_sys_t *sys)
{
	uint32_t res = sys->nextSeqNum++;
	if(sys->nextSeqNum > 2147483647)
		sys->nextSeqNum = 1;
	return res;
}

uint32_t HTSPNextSeqNum(services_discovery_sys_t *sys)
{
	uint32_t res = sys->nextSeqNum++;
	if(sys->nextSeqNum > 2147483647)
		sys->nextSeqNum = 1;
	return res;
}

bool TransmitMessage(demux_t *demux, HtsMessage m)
{
	demux_sys_t *sys = demux->p_sys;

	if(!sys || sys->netfd < 0)
		return false;

	void *buf;
	uint32_t len;

	if(!m.Serialize(&len, &buf))
		return false;

	if(net_Write(demux, sys->netfd, NULL, buf, len) != (ssize_t)len)
		return false;

	free(buf);

	return true;
}

bool TransmitMessage(services_discovery_t *sd, HtsMessage m)
{
	services_discovery_sys_t *sys = sd->p_sys;

	if(!sys || sys->netfd < 0)
		return false;

	void *buf;
	uint32_t len;

	if(!m.Serialize(&len, &buf))
		return false;

	if(net_Write(sd, sys->netfd, NULL, buf, len) != (ssize_t)len)
		return false;

	free(buf);

	return true;
}

HtsMessage ReadMessage(demux_t *demux)
{
	demux_sys_t *sys = demux->p_sys;

	char *buf;
	uint32_t len;

	if(sys->queue.size())
	{
		HtsMessage res = sys->queue.front();
		sys->queue.pop_front();
		return res;
	}

	if(net_Read(demux, sys->netfd, NULL, &len, sizeof(len), false) != sizeof(len))
	{
		msg_Err(demux, "Error reading from socket: %s", strerror(errno));
		return HtsMessage();
	}

	len = ntohl(len);

	if(len == 0)
		return HtsMessage();

	buf = (char*)malloc(len);

	ssize_t read;
	char *wbuf = buf;
	ssize_t tlen = len;
	time_t start = time(0);
	while((read = net_Read(demux, sys->netfd, NULL, wbuf, tlen, false)) < tlen)
	{
		wbuf += read;
		tlen -= read;

		if(difftime(start, time(0)) > READ_TIMEOUT)
		{
			msg_Err(demux, "Read timeout!");
			free(buf);
			return HtsMessage();
		}
	}
	if(read > tlen)
	{
		msg_Dbg(demux, "WTF");
		free(buf);
		return HtsMessage();
	}

	return HtsMessage::Deserialize(len, buf);
}

HtsMessage ReadMessage(services_discovery_t *sd)
{
	services_discovery_sys_t *sys = sd->p_sys;

	char *buf;
	uint32_t len;

	if(sys->queue.size())
	{
		HtsMessage res = sys->queue.front();
		sys->queue.pop_front();
		return res;
	}

	if(net_Read(sd, sys->netfd, NULL, &len, sizeof(len), false) != sizeof(len))
	{
		msg_Err(sd, "Error reading from socket: %s", strerror(errno));
		return HtsMessage();
	}

	len = ntohl(len);

	if(len == 0)
		return HtsMessage();

	buf = (char*)malloc(len);

	ssize_t read;
	char *wbuf = buf;
	ssize_t tlen = len;
	time_t start = time(0);
	while((read = net_Read(sd, sys->netfd, NULL, wbuf, tlen, false)) < tlen)
	{
		wbuf += read;
		tlen -= read;

		if(difftime(start, time(0)) > READ_TIMEOUT)
		{
			msg_Err(sd, "Read timeout!");
			free(buf);
			return HtsMessage();
		}
	}
	if(read > tlen)
	{
		msg_Dbg(sd, "WTF");
		free(buf);
		return HtsMessage();
	}

	return HtsMessage::Deserialize(len, buf);
}

HtsMessage ReadResult(demux_t *demux, HtsMessage m, bool sequence = true)
{
	demux_sys_t *sys = demux->p_sys;

	uint32_t iSequence = 0;
	if(sequence)
	{
		iSequence = HTSPNextSeqNum(sys);
		m.getRoot().setData("seq", iSequence);
	}

	if(!TransmitMessage(demux, m))
		return HtsMessage();

	std::deque<HtsMessage> queue;
	sys->queue.swap(queue);

	while((m = ReadMessage(demux)).isValid())
	{
		if(!sequence)
			break;
		if(m.getRoot().contains("seq") && m.getRoot().getU32("seq") == iSequence)
			break;

		queue.push_back(m);
		if(queue.size() >= MAX_QUEUE_SIZE)
		{
			msg_Dbg(demux, "Max queue size reached!");
			sys->queue.swap(queue);
			return HtsMessage();
		}
	}

	sys->queue.swap(queue);

	if(!m.isValid())
		return HtsMessage();

	if(m.getRoot().contains("error"))
	{
		msg_Err(demux, "HTSP Error: %s", m.getRoot().getStr("error").c_str());
		return HtsMessage();
	}
	if(m.getRoot().getU32("noaccess") != 0)
	{
		msg_Err(demux, "Access Denied");
		return HtsMessage();
	}

	return m;
}

HtsMessage ReadResult(services_discovery_t *sd, HtsMessage m, bool sequence = true)
{
	services_discovery_sys_t *sys = sd->p_sys;

	uint32_t iSequence = 0;
	if(sequence)
	{
		iSequence = HTSPNextSeqNum(sys);
		m.getRoot().setData("seq", iSequence);
	}

	if(!TransmitMessage(sd, m))
		return HtsMessage();

	std::deque<HtsMessage> queue;
	sys->queue.swap(queue);

	while((m = ReadMessage(sd)).isValid())
	{
		if(!sequence)
			break;
		if(m.getRoot().contains("seq") && m.getRoot().getU32("seq") == iSequence)
			break;

		queue.push_back(m);
		if(queue.size() >= MAX_QUEUE_SIZE)
		{
			msg_Dbg(sd, "Max queue size reached!");
			sys->queue.swap(queue);
			return HtsMessage();
		}
	}

	sys->queue.swap(queue);

	if(!m.isValid())
		return HtsMessage();

	if(m.getRoot().contains("error"))
	{
		msg_Err(sd, "HTSP Error: %s", m.getRoot().getStr("error").c_str());
		return HtsMessage();
	}
	if(m.getRoot().getU32("noaccess") != 0)
	{
		msg_Err(sd, "Access Denied");
		return HtsMessage();
	}

	return m;
}

bool ReadSuccess(demux_t *demux, HtsMessage m, const std::string &action, bool sequence = true)
{
	if(!ReadResult(demux, m, sequence).isValid())
	{
		msg_Err(demux, "ReadSuccess - failed to %s", action.c_str());
		return false;
	}
	return true;
}

bool ReadSuccess(services_discovery_t *sd, HtsMessage m, const std::string &action, bool sequence = true)
{
	if(!ReadResult(sd, m, sequence).isValid())
	{
		msg_Err(sd, "ReadSuccess - failed to %s", action.c_str());
		return false;
	}
	return true;
}

/***************************************************
 ****       Initialization Functions            ****
 ***************************************************/

bool ConnectHTSP(demux_t *demux)
{
	demux_sys_t *sys = demux->p_sys;

	sys->netfd = net_ConnectTCP(demux, sys->host.c_str(), sys->port);

	if(sys->netfd < 0)
		return false;

	HtsMap map;
	map.setData("method", "hello");
	map.setData("clientname", "VLC media player");
	map.setData("htspversion", 7);

	HtsMessage m = ReadResult(demux, map.makeMsg());
	if(!m.isValid())
		return false;

	uint32_t chall_len;
	void * chall;

	sys->serverName = m.getRoot().getStr("servername");
	sys->serverVersion = m.getRoot().getStr("serverversion");
	sys->protoVersion = m.getRoot().getU32("htspversion");
	m.getRoot().getBin("challenge", &chall_len, &chall);

	msg_Info(demux, "Connected to HTSP Server %s, version %s, protocol %d", sys->serverName.c_str(), sys->serverVersion.c_str(), sys->protoVersion);

	if(sys->username.empty())
		return true;

	map = HtsMap();
	map.setData("method", "authenticate");
	map.setData("username", sys->username);

	if(sys->password != "" && chall)
	{
		msg_Info(demux, "Authenticating as '%s' with a password", sys->username.c_str());

		HTSSHA1 *shactx = (HTSSHA1*)malloc(hts_sha1_size);
		uint8_t d[20];
		hts_sha1_init(shactx);
		hts_sha1_update(shactx, (const uint8_t *)(sys->password.c_str()), sys->password.length());
		hts_sha1_update(shactx, (const uint8_t *)chall, chall_len);
		hts_sha1_final(shactx, d);

		std::shared_ptr<HtsBin> bin = std::make_shared<HtsBin>();
		bin->setBin(20, d);
		map.setData("digest", bin);

		free(shactx);
	}
	else
		msg_Info(demux, "Authenticating as '%s' without a password", sys->username.c_str());

	if(chall)
		free(chall);

	bool res = ReadSuccess(demux, map.makeMsg(), "authenticate");
	if(res)
		msg_Info(demux, "Successfully authenticated!");
	else
		msg_Err(demux, "Authentication failed!");
	return res;
}

void PopulateEPG(demux_t *demux)
{
	demux_sys_t *sys = demux->p_sys;

	HtsMap map;
	map.setData("method", "getEvents");
	map.setData("channelId", sys->channelId);

	HtsMessage res = ReadResult(demux, map.makeMsg());
	if(!res.isValid())
		return;

	vlc_epg_t *epg = vlc_epg_New(0);

	std::shared_ptr<HtsList> events = res.getRoot().getList("events");
	for(uint32_t i = 0; i < events->count(); i++)
	{
		std::shared_ptr<HtsData> tmp = events->getData(i);
		if(!tmp->isMap())
			continue;
		std::shared_ptr<HtsMap> event = std::static_pointer_cast<HtsMap>(tmp);

		if(event->getU32("channelId") != (uint32_t)sys->channelId)
			continue;

		int64_t start = event->getS64("start");
		int64_t stop = event->getS64("stop");
		int duration = stop - start;

		vlc_epg_AddEvent(epg, start, duration, event->getStr("title").c_str(), event->getStr("summary").c_str(), event->getStr("description").c_str());

		int64_t now = time(0);
		if(now >= start && now < stop)
			vlc_epg_SetCurrent(epg, start);
	}
	es_out_Control(demux->out, ES_OUT_SET_GROUP_EPG, (int)sys->channelId, epg);
	vlc_epg_Delete(epg);
}

bool SubscribeHTSP(demux_t *demux)
{
	demux_sys_t *sys = demux->p_sys;

	HtsMap map;
	map.setData("method", "subscribe");
	map.setData("channelId", sys->channelId);
	map.setData("subscriptionId", 1);
	map.setData("timeshiftPeriod", (uint32_t)~0);
	map.setData("queueDepth", 5*1024*1024);
	//map.setData("90khz", std::make_shared<HtsInt>(1));
	//map.setData("normts", std::make_shared<HtsInt>(1));

	bool res = ReadSuccess(demux, map.makeMsg(), "subscribe to channel");
	if(res)
		msg_Info(demux, "Successfully subscribed to channel %d", sys->channelId);
	return res;
}

bool parseURL(demux_t *demux)
{
	demux_sys_t *sys = demux->p_sys;
	const char *path = demux->psz_location;

	if(path == 0 || *path == 0)
		return false;

	vlc_url_t *url = &(sys->url);
	vlc_UrlParse(url, path, 0);

	if(url->psz_host == 0 || *url->psz_host == 0)
		return false;
	else
		sys->host = url->psz_host;

	if(url->i_port <= 0)
		sys->port = 9982;
	else
		sys->port = url->i_port;

	if(url->psz_username)
		sys->username = url->psz_username;
	if(url->psz_password)
		sys->password = url->psz_password;

	if(url->psz_path == 0 || *(url->psz_path) == '\0' || *(url->psz_path + 1) == '\0')
		sys->channelId = 0;
	else
		sys->channelId = atoi(url->psz_path + 1); // Remove leading '/'

	return true;
}

static int OpenHTSP(vlc_object_t *obj)
{
	demux_t *demux = (demux_t*)obj;

	demux_sys_t *sys = new demux_sys_t;
	if(unlikely(sys == NULL))
		return VLC_ENOMEM;
	demux->p_sys = sys;

	demux->pf_demux = DemuxHTSP;
	demux->pf_control = ControlHTSP;

	msg_Info(demux, "HTSP plugin loading...");

	if(!parseURL(demux))
	{
		msg_Dbg(demux, "Parsing URL failed!");
		CloseHTSP(obj);
		return VLC_EGENERIC;
	}

	if(!ConnectHTSP(demux))
	{
		msg_Dbg(demux, "Connecting to HTS source failed!");
		CloseHTSP(obj);
		return VLC_EGENERIC;
	}

	if(sys->channelId == 0)
	{
		msg_Err(demux, "HTSP ChannelID 0 is invalid!");
		return VLC_EGENERIC;
	}

	PopulateEPG(demux);

	if(!SubscribeHTSP(demux))
	{
		msg_Dbg(demux, "Subscribing to channel failed");
		CloseHTSP(obj);
		return VLC_EGENERIC;
	}

	sys->start = mdate();

	return VLC_SUCCESS;
}

static void CloseHTSP(vlc_object_t *obj)
{
	demux_t *demux = (demux_t*)obj;
	demux_sys_t *sys = demux->p_sys;

	if(!sys)
		return;

	if(sys->netfd >= 0)
		net_Close(sys->netfd);

	delete sys;
	sys = demux->p_sys = 0;
}

static int ControlHTSP(demux_t *demux, int i_query, va_list args)
{
	demux_sys_t *sys = demux->p_sys;

	switch(i_query)
	{
		case DEMUX_CAN_PAUSE:
		case DEMUX_CAN_SEEK:
		case DEMUX_CAN_CONTROL_PACE:
			*va_arg(args, bool*) = false;
			return VLC_SUCCESS;
		case DEMUX_GET_PTS_DELAY:
			*va_arg(args, int64_t*) = INT64_C(1000) * var_InheritInteger(demux, "network-caching") + sys->ptsDelay;
			return VLC_SUCCESS;
		case DEMUX_GET_TIME:
			*va_arg(args, int64_t*) = sys->lastPcr;
			return VLC_SUCCESS;
		default:
			return VLC_EGENERIC;
	}
}

/***************************************************
 ****       Actual Demuxing Work Functions      ****
 ***************************************************/

bool ParseSubscriptionStart(demux_t *demux, HtsMessage &msg)
{
	demux_sys_t *sys = demux->p_sys;

	if(sys->stream != 0)
	{
		for(uint32_t i = 0; i < sys->streamCount; i++)
			es_out_Del(demux->out, sys->stream[i].es);
		delete[] sys->stream;
		sys->stream = 0;
		sys->streamCount = 0;
	}

	if(msg.getRoot().contains("sourceinfo"))
	{
		std::shared_ptr<HtsMap> srcinfo = msg.getRoot().getMap("sourceinfo");

		vlc_meta_t *meta = vlc_meta_New();
		vlc_meta_SetTitle(meta, srcinfo->getStr("service").c_str());
		es_out_Control(demux->out, ES_OUT_SET_GROUP_META, (int)sys->channelId, meta);
		vlc_meta_Delete(meta);
		msg_Dbg(demux, "GOT SRC INFO: %s %s", srcinfo->getStr("adapter").c_str(), srcinfo->getStr("service").c_str());
	}

	std::shared_ptr<HtsList> streams = msg.getRoot().getList("streams");
	if(streams->count() <= 0)
	{
		msg_Err(demux, "Malformed SubscriptionStart!");
		return false;
	}

	sys->streamCount = streams->count();
	msg_Dbg(demux, "Found %d elementary streams", sys->streamCount);

	sys->stream = new hts_stream[sys->streamCount];
	sys->hadIFrame = false;
	sys->pcrStream = 0;
	sys->lastPcr = 0;

	for(uint32_t jj = 0; jj < streams->count(); jj++)
	{
		std::shared_ptr<HtsData> sub = streams->getData(jj);
		if(!sub->isMap())
			continue;
		std::shared_ptr<HtsMap> map = std::static_pointer_cast<HtsMap>(sub);

		std::string type = map->getStr("type");
		if(type.empty())
			continue;

		if(!map->contains("index"))
			continue;
		uint32_t index = map->getU32("index");
		int i = index - 1;

		es_format_t *fmt = &(sys->stream[i].fmt);

		if(type == "AC3")
		{
			es_format_Init(fmt, AUDIO_ES, VLC_CODEC_A52);
		}
		else if(type == "EAC3")
		{
			es_format_Init(fmt, AUDIO_ES, VLC_CODEC_EAC3);
		}
		else if(type == "MPEG2AUDIO")
		{
			es_format_Init(fmt, AUDIO_ES, VLC_CODEC_MPGA);
		}
		else if(type == "AAC")
		{
			es_format_Init(fmt, AUDIO_ES, VLC_CODEC_MP4A);
		}
		else if(type == "AACLATM")
		{
			es_format_Init(fmt, AUDIO_ES, VLC_CODEC_MP4A);
		}
		else if(type == "MPEG2VIDEO")
		{
			es_format_Init(fmt, VIDEO_ES, VLC_CODEC_MP2V);
		}
		else if(type == "H264")
		{
			es_format_Init(fmt, VIDEO_ES, VLC_CODEC_H264);
		}
		else if(type == "DVBSUB")
		{
			es_format_Init(fmt, SPU_ES, VLC_CODEC_DVBS);
		}
		else if(type == "TEXTSUB")
		{
			es_format_Init(fmt, SPU_ES, VLC_CODEC_TEXT);
		}
		else if(type == "TELETEXT")
		{
			es_format_Init(fmt, SPU_ES, VLC_CODEC_TELETEXT);
		}
		else
		{
			sys->stream[i].es = 0;
			continue;
		}

		if(fmt->i_cat == VIDEO_ES)
		{
			if(sys->pcrStream == 0)
				sys->pcrStream = index;

			fmt->video.i_width = map->getU32("width");
			fmt->video.i_height = map->getU32("height");
		}
		else if(fmt->i_cat == AUDIO_ES)
		{
			fmt->audio.i_physical_channels = map->getU32("channels");
			fmt->audio.i_rate = map->getU32("rate");
		}

		std::string lang = map->getStr("language");
		if(!lang.empty())
		{
			fmt->psz_language = (char*)malloc(lang.length()+1);
			strncpy(fmt->psz_language, lang.c_str(), lang.length());
			fmt->psz_language[lang.length()] = 0;
		}

		fmt->i_group = sys->channelId;

		sys->stream[i].es = es_out_Add(demux->out, fmt);

		msg_Dbg(demux, "Found elementary stream id %d, type %s", index, type.c_str());
	}

	if(sys->pcrStream == 0)
		for(uint32_t i = 0; i < sys->streamCount; i++)
			if(sys->stream[i].fmt.i_cat == AUDIO_ES)
				sys->pcrStream = i+1;
	if(sys->pcrStream == 0)
		sys->pcrStream = 1;

	return true;
}

bool ParseSubscriptionStop(demux_t *demux, HtsMessage &msg)
{
	msg_Info(demux, "HTS Subscription Stop: subscriptionId: %d, status: %s", msg.getRoot().getU32("subscriptionId"), msg.getRoot().getStr("status").c_str());
	return false;
}

bool ParseSubscriptionStatus(demux_t *demux, HtsMessage &msg)
{
	msg_Dbg(demux, "HTS Subscription Status: subscriptionId: %d, status: %s", msg.getRoot().getU32("subscriptionId"), msg.getRoot().getStr("status").c_str());
	return true;
}

bool ParseQueueStatus(demux_t *demux, HtsMessage &msg)
{
	demux_sys_t *sys = demux->p_sys;

	uint32_t drops = msg.getRoot().getU32("Bdrops") + msg.getRoot().getU32("Pdrops") + msg.getRoot().getU32("Idrops");
	if(drops > sys->drops)
	{

		msg_Warn(demux, "Can't keep up! HTS dropped %d frames!", drops - sys->drops);
		msg_Warn(demux, "HTS Queue Status: subscriptionId: %d, Packets: %d, Bytes: %d, Delay: %lld, Bdrops: %d, Pdrops: %d, Idrops: %d",
			msg.getRoot().getU32("subscriptionId"),
			msg.getRoot().getU32("packets"),
			msg.getRoot().getU32("bytes"),
			(long long int)msg.getRoot().getS64("delay"),
			msg.getRoot().getU32("Bdrops"),
			msg.getRoot().getU32("Pdrops"),
			msg.getRoot().getU32("Idrops"));

		sys->drops += drops;
	}
	else
	{
		msg_Dbg(demux, "HTS Queue Status: subscriptionId: %d, Packets: %d, Bytes: %d, Delay: %lld, Bdrops: %d, Pdrops: %d, Idrops: %d",
			msg.getRoot().getU32("subscriptionId"),
			msg.getRoot().getU32("packets"),
			msg.getRoot().getU32("bytes"),
			(long long int)msg.getRoot().getS64("delay"),
			msg.getRoot().getU32("Bdrops"),
			msg.getRoot().getU32("Pdrops"),
			msg.getRoot().getU32("Idrops"));
	}
	return true;
}

bool ParseSignalStatus(demux_t *demux, HtsMessage &msg)
{
	VLC_UNUSED(demux);
	VLC_UNUSED(msg);
	return true;
}

bool ParseMuxPacket(demux_t *demux, HtsMessage &msg)
{
	demux_sys_t *sys = demux->p_sys;

	uint32_t index = msg.getRoot().getU32("stream");

	void *bin = 0;
	uint32_t binlen = 0;
	msg.getRoot().getBin("payload", &binlen, &bin);

	int64_t pts = 0;
	int64_t dts = 0;

	uint32_t frametype = 0;

	if(bin == 0)
	{
		msg_Err(demux, "Malformed Mux Packet!");
		return false;
	}

	if(index == 0 || binlen == 0)
	{
		free(bin);
		msg_Err(demux, "Malformed Mux Packet!");
		return false;
	}

	if(index > sys->streamCount || index == 0)
	{
		free(bin);
		msg_Err(demux, "Invalid stream index detected: %d with %d streams", index, sys->streamCount);
		return false;
	}

	if(sys->stream[index - 1].es == 0)
	{
		free(bin);
		return true;
	}

	block_t *block = block_Alloc(binlen);
	if(unlikely(block == 0))
	{
		free(bin);
		return false;
	}

	memcpy(block->p_buffer, bin, binlen);
	free(bin);
	bin = 0;

	pts = block->i_pts = VLC_TS_INVALID;
	if(msg.getRoot().contains("pts"))
		pts = block->i_pts = msg.getRoot().getS64("pts");

	dts = block->i_dts = VLC_TS_INVALID;
	if(msg.getRoot().contains("dts"))
		dts = block->i_dts = msg.getRoot().getS64("dts");

	int64_t duration = msg.getRoot().getS64("duration");
	if(duration != 0)
		block->i_length = duration;

	if(pts > 0)
		sys->stream[index - 1].lastPts = pts;
	if(dts > 0)
		sys->stream[index - 1].lastDts = dts;

	frametype = msg.getRoot().getU32("frametype");
	if(sys->stream[index - 1].fmt.i_cat == VIDEO_ES && frametype != 0)
	{
		char ft = (char)frametype;

		if(!sys->hadIFrame && ft != 'I')
		{
			block_Release(block);
			free(bin);
			return true;
		}

		if(ft == 'I')
		{
			sys->hadIFrame = true;
			block->i_flags = BLOCK_FLAG_TYPE_I;
		}
		else if(ft == 'B')
			block->i_flags = BLOCK_FLAG_TYPE_B;
		else if(ft == 'P')
			block->i_flags = BLOCK_FLAG_TYPE_P;
	}

	mtime_t pcr = 0;
	mtime_t bpcr = 0;
	for(uint32_t i = 0; i < sys->streamCount; i++)
	{
		if(sys->stream[i].lastDts > 0 && (sys->stream[i].lastDts < pcr || pcr == 0))
		{
			pcr = sys->stream[i].lastDts;
		}
		if(sys->stream[i].lastPts > 0 && (sys->stream[i].lastDts > bpcr || bpcr == 0))
		{
			bpcr = sys->stream[i].lastDts;
		}
	}

	if(pcr > sys->lastPcr + sys->ptsDelay && pcr > 0)
	{
		es_out_Control(demux->out, ES_OUT_SET_PCR, VLC_TS_0 + pcr);
		sys->lastPcr = pcr;
		//msg_Dbg(demux, "Sent PCR %ld from stream %d, biggest current pcr is %ld from stream %d, diff %ld with a delay of %ld", pcr, nbstream, bpcr, bstream, bpcr - pcr, sys->ptsDelay);
	}

	es_out_Send(demux->out, sys->stream[index - 1].es, block);

	return true;
}

static int DemuxHTSP(demux_t *demux)
{
	demux_sys_t *sys = demux->p_sys;
	if(sys->channelId == 0)
		return DEMUX_EOF;

	HtsMessage msg = ReadMessage(demux);
	if(!msg.isValid())
		return DEMUX_EOF;

	std::string method = msg.getRoot().getStr("method");
	if(method.empty())
		return DEMUX_ERROR;

	uint32_t subs = msg.getRoot().getU32("subscriptionId");
	if(subs != 1)
		return DEMUX_OK;

	if(method == "subscriptionStart")
	{
		if(!ParseSubscriptionStart(demux, msg))
		{
			return DEMUX_ERROR;
		}
	}
	else if(method == "subscriptionStop")
	{
		if(!ParseSubscriptionStop(demux, msg))
		{
			return DEMUX_ERROR;
		}
	}
	else if(method == "subscriptionStatus")
	{
		if(!ParseSubscriptionStatus(demux, msg))
		{
			return DEMUX_ERROR;
		}
	}
	else if(method == "queueStatus")
	{
		if(!ParseQueueStatus(demux, msg))
		{
			return DEMUX_ERROR;
		}
	}
	else if(method == "signalStatus")
	{
		if(!ParseSignalStatus(demux, msg))
		{
			return DEMUX_ERROR;
		}
	}
	else if(method == "muxpkt")
	{
		if(!ParseMuxPacket(demux, msg))
		{
			return DEMUX_ERROR;
		}
	}
	else
	{
		msg_Dbg(demux, "Ignoring packet of unknown method \"%s\"", method.c_str());
	}

	return DEMUX_OK;
}

/***************************************************
 ****       Services Discovery Functions        ****
 ***************************************************/

bool ConnectHTSP(services_discovery_t *sd)
{
	services_discovery_sys_t *sys = sd->p_sys;

	const char *host = var_GetString(sd, CFG_PREFIX"host");
	int port = var_GetInteger(sd, CFG_PREFIX"port");

	if(host == 0 || host[0] == 0)
		host = "localhost";

	if(port == 0)
		port = 9982;

	sys->netfd = net_ConnectTCP(sd, host, port);

	if(sys->netfd < 0)
	{
		msg_Err(sd, "net_ConnectTCP failed");
		return false;
	}

	HtsMap map;
	map.setData("method", "hello");
	map.setData("clientname", "VLC media player");
	map.setData("htspversion", 7);

	HtsMessage m = ReadResult(sd, map.makeMsg());
	if(!m.isValid())
	{
		msg_Err(sd, "No valid hello response");
		return false;
	}

	uint32_t chall_len;
	void * chall;
	m.getRoot().getBin("challenge", &chall_len, &chall);

	std::string serverName = m.getRoot().getStr("servername");
	std::string serverVersion = m.getRoot().getStr("serverversion");
	uint32_t protoVersion = m.getRoot().getU32("htspversion");

	msg_Info(sd, "Connected to HTSP Server %s, version %s, protocol %d", serverName.c_str(), serverVersion.c_str(), protoVersion);

	const char *user = var_GetString(sd, CFG_PREFIX"user");
	const char *pass = var_GetString(sd, CFG_PREFIX"pass");
	if(user == 0 || user[0] == 0)
		return true;

	map = HtsMap();
	map.setData("method", "authenticate");
	map.setData("username", user);

	if(pass != 0 && pass[0] != 0 && chall)
	{
		msg_Info(sd, "Authenticating as '%s' with a password", user);

		HTSSHA1 *shactx = (HTSSHA1*)malloc(hts_sha1_size);
		uint8_t d[20];
		hts_sha1_init(shactx);
		hts_sha1_update(shactx, (const uint8_t *)pass, strlen(pass));
		hts_sha1_update(shactx, (const uint8_t *)chall, chall_len);
		hts_sha1_final(shactx, d);

		std::shared_ptr<HtsBin> bin = std::make_shared<HtsBin>();
		bin->setBin(20, d);
		map.setData("digest", bin);

		free(shactx);
	}
	else
		msg_Info(sd, "Authenticating as '%s' without a password", user);

	if(chall)
		free(chall);

	bool res = ReadSuccess(sd, map.makeMsg(), "authenticate");
	if(res)
		msg_Info(sd, "Successfully authenticated!");
	else
		msg_Err(sd, "Authentication failed!");
	return res;
}

bool compare_tmp_channel(tmp_channel first, tmp_channel second)
{
	if(first.cnum < second.cnum)
		return true;
	return false;
}

bool GetChannels(services_discovery_t *sd)
{
	services_discovery_sys_t *sys = sd->p_sys;

	HtsMap map;
	map.setData("method", "enableAsyncMetadata");
	if(!ReadSuccess(sd, map.makeMsg(), "enable async metadata"))
		return false;

	std::list<tmp_channel> channels;

	HtsMessage m;
	while((m = ReadMessage(sd)).isValid())
	{
		std::string method = m.getRoot().getStr("method");
		if(method.empty() || method == "initialSyncCompleted")
		{
			msg_Info(sd, "Finished getting initial metadata sync");
			break;
		}

		if(method == "channelAdd")
		{
			if(!m.getRoot().contains("channelId"))
				continue;
			uint32_t cid = m.getRoot().getU32("channelId");

			std::string cname = m.getRoot().getStr("channelName");
			if(cname.empty())
				continue;

			if(!m.getRoot().contains("channelNumber"))
				continue;
			uint32_t cnum = m.getRoot().getU32("channelNumber");

			std::ostringstream oss;
			oss << "htsp://";

			char *user = var_GetString(sd, CFG_PREFIX"user");
			char *pass = var_GetString(sd, CFG_PREFIX"pass");
			if(user != 0 && user[0] != 0 && pass != 0 && pass[0] != 0)
				oss << user << ":" << pass << "@";
			else if(user != 0 && user[0] != 0)
				oss << user << "@";

			const char *host = var_GetString(sd, CFG_PREFIX"host");
			if(host == 0 || host[0] == 0)
				host = "localhost";
			int port = var_GetInteger(sd, CFG_PREFIX"port");
			if(port == 0)
				port = 9982;
			oss << host << ":" << port << "/" << cid;

			tmp_channel ch;
			ch.name = cname;
			ch.cid = cid;
			ch.cnum = cnum;
			ch.url = oss.str();
			channels.push_back(ch);
		}
	}

	channels.sort(compare_tmp_channel);

	while(channels.size() > 0)
	{
		tmp_channel ch = channels.front();
		channels.pop_front();

		ch.item = input_item_New(ch.url.c_str(), ch.name.c_str());
		if(unlikely(ch.item == 0))
			return false;

		services_discovery_AddItem(sd, ch.item, "Channels");

		sys->channelMap[ch.cid] = ch;
	}

	return true;
}

void * RunSD(void *obj)
{
	services_discovery_t *sd = (services_discovery_t *)obj;

	GetChannels(sd);

	for(;;)
	{
		HtsMessage msg = ReadMessage(sd);
		if(!msg.isValid())
			return 0;

		std::string method = msg.getRoot().getStr("method");
		if(method.empty())
			return 0;

		msg_Dbg(sd, "Got Message with method %s", method.c_str());
	}

	return 0;
}

static int OpenSD(vlc_object_t *obj)
{
	services_discovery_t *sd = (services_discovery_t *)obj;
	services_discovery_sys_t *sys = new services_discovery_sys_t;
	if(unlikely(sys == NULL))
		return VLC_ENOMEM;
	sd->p_sys = sys;

	config_ChainParse(sd, CFG_PREFIX, cfg_options, sd->p_cfg);

	if(!ConnectHTSP(sd))
	{
		msg_Err(sd, "Connecting to HTS Failed!");
		return VLC_EGENERIC;
	}

	if(vlc_clone(&sys->thread, RunSD, sd, VLC_THREAD_PRIORITY_LOW))
	{
		net_Close(sys->netfd);
		delete sys;
		return VLC_EGENERIC;
	}

	return VLC_SUCCESS;
}

static void CloseSD(vlc_object_t *obj)
{
	services_discovery_t *sd = (services_discovery_t *)obj;
	services_discovery_sys_t *sys = sd->p_sys;

	if(!sys)
		return;

	vlc_cancel(sys->thread);
	vlc_join(sys->thread, 0);

	if(sys->netfd >= 0)
		net_Close(sys->netfd);

	delete sys;
	sys = sd->p_sys = 0;
}
