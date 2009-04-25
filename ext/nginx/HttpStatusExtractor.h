/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2009 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */

#include <string>
#include <map>

namespace Passenger {

using namespace std;

/**
 * Mapping of common HTTP status codes and their status messages.
 * Stolen from Rack, which stole it from Mongrel.
 */
static pair<string,string> httpStatusCodes[] = {
  pair<string,string>("100","Continue"),
  pair<string,string>("101","Switching Protocols"),
  pair<string,string>("200","OK"),
  pair<string,string>("201","Created"),
  pair<string,string>("202","Accepted"),
  pair<string,string>("203","Non-Authoritative Information"),
  pair<string,string>("204","No Content"),
  pair<string,string>("205","Reset Content"),
  pair<string,string>("206","Partial Content"),
  pair<string,string>("300","Multiple Choices"),
  pair<string,string>("301","Moved Permanently"),
  pair<string,string>("302","Found"),
  pair<string,string>("303","See Other"),
  pair<string,string>("304","Not Modified"),
  pair<string,string>("305","Use Proxy"),
  pair<string,string>("307","Temporary Redirect"),
  pair<string,string>("400","Bad Request"),
  pair<string,string>("401","Unauthorized"),
  pair<string,string>("402","Payment Required"),
  pair<string,string>("403","Forbidden"),
  pair<string,string>("404","Not Found"),
  pair<string,string>("405","Method Not Allowed"),
  pair<string,string>("406","Not Acceptable"),
  pair<string,string>("407","Proxy Authentication Required"),
  pair<string,string>("408","Request Timeout"),
  pair<string,string>("409","Conflict"),
  pair<string,string>("410","Gone"),
  pair<string,string>("411","Length Required"),
  pair<string,string>("412","Precondition Failed"),
  pair<string,string>("413","Request Entity Too Large"),
  pair<string,string>("414","Request-URI Too Large"),
  pair<string,string>("415","Unsupported Media Type"),
  pair<string,string>("416","Requested Range Not Satisfiable"),
  pair<string,string>("417","Expectation Failed"),
  pair<string,string>("500","Internal Server Error"),
  pair<string,string>("501","Not Implemented"),
  pair<string,string>("502","Bad Gateway"),
  pair<string,string>("503","Service Unavailable"),
  pair<string,string>("504","Gateway Timeout"),
  pair<string,string>("505","HTTP Version Not Supported")
};

static map<string,string> httpStatusCodesMap(httpStatusCodes, httpStatusCodes + sizeof(httpStatusCodes) / sizeof(httpStatusCodes[0]));

/**
 * Utility class for extracting the HTTP status value from an HTTP response.
 *
 * This class is used for generating a proper HTTP response. The response data
 * that Passenger backend processes generate are like CGI responses, and do not
 * include an initial "HTTP/1.1 [status here]" line, so this class used to
 * extract the status from the response in order to generate a proper initial
 * HTTP response line.
 *
 * This class is supposed to be used as follows:
 * - Keep feeding HTTP response data until feed() returns true. feed()
 *   buffers all fed data until it is able to extract the HTTP status.
 * - Call getStatusLine() to retrieve the status line, and use this to generate
 *   an HTTP response line.
 * - Call getBuffer() to retrieve all fed data so far. This data can be sent to
 *   the HTTP client.
 *
 * @note
 * When the API documentation for this class refers to "\r\n", we actually
 * mean "\x0D\x0A" (the HTTP line termination string). "\r\n" is only written
 * out of convenience.
 */
class HttpStatusExtractor {
private:
	static const char CR = '\x0D';
	static const char LF = '\x0A';

	string buffer;
	unsigned int searchStart;
	bool fullHeaderReceived;
	string statusLine;
	
	bool extractStatusLine() {
		static const char statusHeaderName[] = "Status: ";
		string::size_type start_pos, newline_pos;
		
		if (buffer.size() > sizeof(statusHeaderName) - 1
		 && memcmp(buffer.c_str(), statusHeaderName, sizeof(statusHeaderName) - 1) == 0) {
			// Status line starts at beginning of the header.
			start_pos = sizeof(statusHeaderName) - 1;
			newline_pos = buffer.find("\x0D\x0A", 0, 2) + 2;
		} else {
			// Status line is not at the beginning of the header.
			// Look for it.
			start_pos = buffer.find("\x0D\x0AStatus: ");
			if (start_pos != string::npos) {
				start_pos += 2 + sizeof(statusHeaderName) - 1;
				newline_pos = buffer.find("\x0D\x0A", start_pos, 2) + 2;
			}
		}
		if (start_pos != string::npos) {
			// Status line has been found. Extract it.
			statusLine = buffer.substr(start_pos, newline_pos - start_pos);

			// Pull out the status code from the status header line and look it up from the 
			// status code map. When a match is found construct a partial HTTP/1.1 compliant
			// status line in this format. Note that the "HTTP-Version SP" prefix will be
			// appended by the caller. Format and example:
			// statusLine = Status-Code SP Reason-Phrase CRLF
			// examples: "200 OK\r\n", "304 Not Modified\r\n"
			map<string,string>::iterator iter = httpStatusCodesMap.find(statusLine.substr(0, 3));
			if( iter != httpStatusCodesMap.end() ) {
				// when a match is found overwrite the statusLine and concat the code with 
				// the message and the trailing CRLF
				statusLine = iter->first; // Status-Code
				statusLine.append(" ");   // SP
				statusLine.append(iter->second);     // Reason-Phrase
				statusLine.append("\r\n");           // CRLF
			}
			return true;
		} else {
			// Status line is not found. Do not change default
			// status line value.
			return false;
		}
	}
	
public:
	HttpStatusExtractor() {
		searchStart = 0;
		fullHeaderReceived = false;
		statusLine = "200 OK\r\n";
	}
	
	/**
	 * Feed HTTP response data to this HttpStatusExtractor.
	 *
	 * One is to keep feeding data until this method returns true.
	 * When a sufficient amount of data has been fed, this method will
	 * extract the status line from the data that has been fed so far,
	 * and return true.
	 *
	 * Do not call this method again once it has returned true.
	 *
	 * It is safe to feed excess data. That is, it is safe if the 'data'
	 * argument contains a part of the HTTP response body.
	 * HttpStatusExtractor will only look for the status line in the HTTP
	 * response header, not in the HTTP response body. All fed data is
	 * buffered and will be available via getBuffer(), so no data will be
	 * lost.
	 *
	 * @return Whether the HTTP status has been extracted yet.
	 * @pre feed() did not previously return true.
	 * @pre data != NULL
	 * @pre size > 0
	 */
	bool feed(const char *data, unsigned int size) {
		if (fullHeaderReceived) {
			return true;
		}
		buffer.append(data, size);
		for (; buffer.size() >= 3 && searchStart < buffer.size() - 3; searchStart++) {
			if (buffer[searchStart] == CR &&
			    buffer[searchStart + 1] == LF &&
			    buffer[searchStart + 2] == CR &&
			    buffer[searchStart + 3] == LF) {
				fullHeaderReceived = true;
				extractStatusLine();
				return true;
			}
		}
		return false;
	}
	
	/**
	 * Returns the HTTP status line that has been determined.
	 *
	 * The default value is "200 OK\r\n", which is returned if the HTTP
	 * response data that has been fed so far does not include a status
	 * line.
	 *
	 * @note The return value includes a trailing CRLF, e.g. "404 Not Found\r\n".
	 */
	string getStatusLine() const {
		return statusLine;
	}
	
	/**
	 * Get the data that has been fed so far.
	 */
	string getBuffer() const {
		return buffer;
	}
};

} // namespace Passenger
