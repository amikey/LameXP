///////////////////////////////////////////////////////////////////////////////
// LameXP - Audio Encoder Front-End
// Copyright (C) 2004-2015 LoRd_MuldeR <MuldeR2@GMX.de>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version, but always including the *additional*
// restrictions defined in the "License.txt" file.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
// http://www.gnu.org/licenses/gpl-2.0.txt
///////////////////////////////////////////////////////////////////////////////

#include "Encoder_AAC_FDK.h"

//Internal
#include "Global.h"
#include "Model_Settings.h"

//MUtils
#include <MUtils/Global.h>

//StdLib
#include <math.h>

//Qt
#include <QProcess>
#include <QDir>
#include <QCoreApplication>

static int index2bitrate(const int index)
{
	return (index < 32) ? ((index + 1) * 8) : ((index - 15) * 16);
}

///////////////////////////////////////////////////////////////////////////////
// Encoder Info
///////////////////////////////////////////////////////////////////////////////

class FDKAACEncoderInfo : public AbstractEncoderInfo
{
	virtual bool isModeSupported(int mode) const
	{
		switch(mode)
		{
		case SettingsModel::VBRMode:
		case SettingsModel::CBRMode:
			return true;
			break;
		case SettingsModel::ABRMode:
			return false;
			break;
		default:
			MUTILS_THROW("Bad RC mode specified!");
		}
	}

	virtual int valueCount(int mode) const
	{
		switch(mode)
		{
		case SettingsModel::VBRMode:
			return 5;
			break;
		case SettingsModel::CBRMode:
			return 52;
			break;
		default:
			MUTILS_THROW("Bad RC mode specified!");
		}
	}

	virtual int valueAt(int mode, int index) const
	{
		switch(mode)
		{
		case SettingsModel::VBRMode:
			return qBound(1, index + 1 , 5);
			break;
		case SettingsModel::CBRMode:
			return qBound(8, index2bitrate(index), 576);
			break;
		default:
			MUTILS_THROW("Bad RC mode specified!");
		}
	}

	virtual int valueType(int mode) const
	{
		switch(mode)
		{
		case SettingsModel::VBRMode:
			return TYPE_QUALITY_LEVEL_INT;
			break;
		case SettingsModel::CBRMode:
			return TYPE_BITRATE;
			break;
		default:
			MUTILS_THROW("Bad RC mode specified!");
		}
	}

	virtual const char *description(void) const
	{
		static const char* s_description = "fdkaac (libfdk-aac encoder)";
		return s_description;
	}

	virtual const char *extension(void) const
	{
		static const char* s_extension = "mp4";
		return s_extension;
	}
}
static const g_fdkAacEncoderInfo;

///////////////////////////////////////////////////////////////////////////////
// Encoder implementation
///////////////////////////////////////////////////////////////////////////////

FDKAACEncoder::FDKAACEncoder(void)
:
	m_binary(lamexp_tools_lookup("fdkaac.exe"))
{
	if(m_binary.isEmpty())
	{
		MUTILS_THROW("Error initializing FDKAAC. Tool 'fdkaac.exe' is not registred!");
	}

	m_configProfile = 0;
}

FDKAACEncoder::~FDKAACEncoder(void)
{
}

bool FDKAACEncoder::encode(const QString &sourceFile, const AudioFileModel_MetaInfo &metaInfo, const unsigned int duration, const QString &outputFile, volatile bool *abortFlag)
{
	QProcess process;
	QStringList args;

	process.setWorkingDirectory(QFileInfo(outputFile).canonicalPath());

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert("PATH", QDir::toNativeSeparators(QString("%1;%1/QTfiles;%2").arg(QDir(QCoreApplication::applicationDirPath()).canonicalPath(), MUtils::temp_folder())));
	process.setProcessEnvironment(env);

	switch(m_configProfile)
	{
	case 1:
		args << "-p" << "2";
		break;
	case 2:
		args << "-p" << "5";
		break;
	case 3:
		args << "-p" << "29";
		break;
	}

	switch(m_configRCMode)
	{
	case SettingsModel::CBRMode:
		args << "-b" << QString::number(qBound(8, index2bitrate(m_configBitrate), 576));
		break;
	case SettingsModel::VBRMode:
		args << "-m" << QString::number(qBound(1, m_configBitrate + 1 , 5));
		break;
	default:
		MUTILS_THROW("Bad rate-control mode!");
		break;
	}

	if(!m_configCustomParams.isEmpty()) args << m_configCustomParams.split(" ", QString::SkipEmptyParts);

	if(!metaInfo.title().isEmpty())   args << "--title"   << cleanTag(metaInfo.title());
	if(!metaInfo.artist().isEmpty())  args << "--artist"  << cleanTag(metaInfo.artist());
	if(!metaInfo.album().isEmpty())   args << "--album"   << cleanTag(metaInfo.album());
	if(!metaInfo.genre().isEmpty())   args << "--genre"   << cleanTag(metaInfo.genre());
	if(!metaInfo.comment().isEmpty()) args << "--comment" << cleanTag( metaInfo.comment());
	if(metaInfo.year())               args << "--date"    << QString::number(metaInfo.year());
	if(metaInfo.position())           args << "--track"   << QString::number(metaInfo.position());

	args << "-o" << QDir::toNativeSeparators(outputFile);
	args << QDir::toNativeSeparators(sourceFile);

	if(!startProcess(process, m_binary, args))
	{
		return false;
	}

	bool bTimeout = false;
	bool bAborted = false;
	int prevProgress = -1;

	QRegExp regExp("\\[(\\d+)%\\]\\s+(\\d+):(\\d+)");

	while(process.state() != QProcess::NotRunning)
	{
		if(*abortFlag)
		{
			process.kill();
			bAborted = true;
			emit messageLogged("\nABORTED BY USER !!!");
			break;
		}
		process.waitForReadyRead(m_processTimeoutInterval);
		if(!process.bytesAvailable() && process.state() == QProcess::Running)
		{
			process.kill();
			qWarning("FDKAAC process timed out <-- killing!");
			emit messageLogged("\nPROCESS TIMEOUT !!!");
			bTimeout = true;
			break;
		}
		while(process.bytesAvailable() > 0)
		{
			QByteArray line = process.readLine();
			QString text = QString::fromUtf8(line.constData()).simplified();
			if(regExp.lastIndexIn(text) >= 0)
			{
				bool ok = false;
				int progress = regExp.cap(1).toInt(&ok);
				if(ok && (progress > prevProgress))
				{
					emit statusUpdated(progress);
					prevProgress = qMin(progress + 2, 99);
				}
			}
			else if(!text.isEmpty())
			{
				emit messageLogged(text);
			}
		}
	}

	process.waitForFinished();
	if(process.state() != QProcess::NotRunning)
	{
		process.kill();
		process.waitForFinished(-1);
	}
	
	emit statusUpdated(100);
	emit messageLogged(QString().sprintf("\nExited with code: 0x%04X", process.exitCode()));

	if(bTimeout || bAborted || process.exitCode() != EXIT_SUCCESS)
	{
		return false;
	}

	return true;
}

bool FDKAACEncoder::isFormatSupported(const QString &containerType, const QString &containerProfile, const QString &formatType, const QString &formatProfile, const QString &formatVersion)
{
	if(containerType.compare("Wave", Qt::CaseInsensitive) == 0)
	{
		if(formatType.compare("PCM", Qt::CaseInsensitive) == 0)
		{
			return true;
		}
	}

	return false;
}

void FDKAACEncoder::setProfile(int profile)
{
	m_configProfile = profile;
}

const AbstractEncoderInfo *FDKAACEncoder::getEncoderInfo(void)
{
	return &g_fdkAacEncoderInfo;
}