/*
	grive: an GPL program to sync a local directory with Google Drive
	Copyright (C) 2012  Wan Wai Ho

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation version 2
	of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "base/Resource.hh"
#include "CommonUri.hh"
#include "Entry1.hh"
#include "Syncer1.hh"

#include "http/Agent.hh"
#include "http/Download.hh"
#include "http/Header.hh"
//#include "http/ResponseLog.hh"
#include "http/StringResponse.hh"
#include "http/XmlResponse.hh"

#include "xml/Node.hh"
#include "xml/NodeSet.hh"
#include "xml/String.hh"
#include "xml/TreeBuilder.hh"

#include "util/OS.hh"
#include "util/log/Log.hh"

#include <boost/exception/all.hpp>

#include <cassert>

// for debugging
#include <iostream>

namespace gr { namespace v1 {

// hard coded XML file
const std::string xml_meta =
	"<?xml version='1.0' encoding='UTF-8'?>\n"
	"<entry xmlns=\"http://www.w3.org/2005/Atom\" xmlns:docs=\"http://schemas.google.com/docs/2007\">"
		"<category scheme=\"http://schemas.google.com/g/2005#kind\" "
		"term=\"http://schemas.google.com/docs/2007#%1%\"/>"
		"<title>%2%</title>"
	"</entry>" ;

Syncer1::Syncer1( http::Agent *http ):
	Syncer( http )
{
}

void Syncer1::DeleteRemote( Resource *res )
{
	http::StringResponse str ;
	
	try
	{
		http::Header hdr ;
		hdr.Add( "If-Match: " + res->ETag() ) ;
		
		// don't know why, but an update before deleting seems to work always
		http::XmlResponse xml ;
		m_http->Get( res->SelfHref(), &xml, hdr ) ;
		AssignIDs( res, Entry1( xml.Response() ) ) ;
		
		m_http->Custom( "DELETE", res->SelfHref(), &str, hdr ) ;
	}
	catch ( Exception& e )
	{
		// don't rethrow here. there are some cases that I don't know why
		// the delete will fail.
		Trace( "Exception %1% %2%",
			boost::diagnostic_information(e),
			str.Response() ) ;
	}
}

void Syncer1::Download( Resource *res, const fs::path& file )
{
	http::Download dl( file.string(), http::Download::NoChecksum() ) ;
	long r = m_http->Get( res->ContentSrc(), &dl, http::Header() ) ;
	if ( r <= 400 )
	{
		if ( res->MTime() != DateTime() )
			os::SetFileTime( file, res->MTime() ) ;
		else
			Log( "encountered zero date time after downloading %1%", file, log::warning ) ;
	}
}

bool Syncer1::EditContent( Resource *res, bool new_rev )
{
	assert( res->Parent() ) ;
	assert( res->Parent()->GetState() == Resource::sync ) ;

	if ( !res->IsEditable() )
	{
		Log( "Cannot upload %1%: file read-only. %2%", res->Name(), res->StateStr(), log::warning ) ;
		return false ;
	}

	return Upload( res, feed_base + "/" + res->ResourceID() + ( new_rev ? "?new-revision=true" : "" ), false ) ;
}

bool Syncer1::Create( Resource *res )
{
	assert( res->Parent() ) ;
	assert( res->Parent()->IsFolder() ) ;
	assert( res->Parent()->GetState() == Resource::sync ) ;
	
	if ( res->IsFolder() )
	{
		std::string uri = feed_base ;
		if ( !res->Parent()->IsRoot() )
			uri += ( "/" + m_http->Escape( res->Parent()->ResourceID() ) + "/contents" ) ;
		
		std::string meta = (boost::format( xml_meta )
			% "folder"
			% xml::Escape( res->Name() )
		).str() ;

		http::Header hdr ;
		hdr.Add( "Content-Type: application/atom+xml" ) ;

		http::XmlResponse xml ;
//		http::ResponseLog log( "create", ".xml", &xml ) ;
		m_http->Post( uri, meta, &xml, hdr ) ;
		AssignIDs( res, Entry1( xml.Response() ) ) ;

		return true ;
	}
	else if ( res->Parent()->IsEditable() )
	{
		return Upload( res, root_create + (res->Parent()->ResourceID() == "folder:root"
			? "" : "/" + res->Parent()->ResourceID() + "/contents") + "?convert=false", true ) ;
	}
	else
	{
		Log( "parent of %1% does not exist: cannot upload", res->Name(), log::warning ) ;
		return false ;
	}
}

bool Syncer1::Upload( Resource *res,
	const std::string&	link,
	bool 				post )
{
	File file( res->Path() ) ;
	std::ostringstream xcontent_len ;
	xcontent_len << "X-Upload-Content-Length: " << file.Size() ;

	http::Header hdr ;
	hdr.Add( "Content-Type: application/atom+xml" ) ;
	hdr.Add( "X-Upload-Content-Type: application/octet-stream" ) ;
	hdr.Add( xcontent_len.str() ) ;
	hdr.Add( "If-Match: " + res->ETag() ) ;
	hdr.Add( "Expect:" ) ;

	std::string meta = (boost::format( xml_meta )
		% res->Kind()
		% xml::Escape( res->Name() )
	).str() ;
	
	bool retrying = false;
	while ( true )
	{
		if ( retrying )
		{
			file.Seek( 0, SEEK_SET );
			os::Sleep( 2 );
		}

		try
		{
			http::StringResponse str ;
			if ( post )
				m_http->Post( link, meta, &str, hdr ) ;
			else
				m_http->Put( link, meta, &str, hdr ) ;
		}
		catch ( Exception &e )
		{
			std::string const *info = boost::get_error_info<xml::TreeBuilder::ExpatApiError>(e);
			if ( info && (*info == "XML_Parse") )
			{
				Log( "Error parsing pre-upload response XML, retrying whole upload in 5s",
						log::warning );
				retrying = true;
				continue;
			}
			else
			{
				throw e;
			}
		}

		http::Header uphdr ;
		uphdr.Add( "Expect:" ) ;
		uphdr.Add( "Accept:" ) ;

		// the content upload URL is in the "Location" HTTP header
		std::string uplink = m_http->RedirLocation() ;
		http::XmlResponse xml ;

		long http_code = 0;
		try
		{
			http_code = m_http->Put( uplink, &file, &xml, uphdr ) ;
		}
		catch ( Exception &e )
		{
			std::string const *info = boost::get_error_info<xml::TreeBuilder::ExpatApiError>(e);
			if ( info && (*info == "XML_Parse") )
			{
				Log( "Error parsing response XML, retrying whole upload in 5s",
						log::warning );
				retrying = true;
				continue;
			}
			else
			{
				throw e;
			}
		}

		if ( http_code == 410 || http_code == 412 )
		{
			Log( "request failed with %1%, retrying whole upload in 5s", http_code, log::warning ) ;
			retrying = true;
			continue;
		}

		if ( retrying )
			Log( "upload succeeded on retry", log::warning );
		Entry1 responseEntry = Entry1( xml.Response() );
		AssignIDs( res, responseEntry ) ;
		AssignMTime( res, responseEntry.MTime() );
		break;
	}
	
	return true ;
}

} } // end of namespace gr::v1