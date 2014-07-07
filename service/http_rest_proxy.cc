/* http_rest_proxy.cc
   Jeremy Barnes, 10 April 2013
   Copyright (c) 2013 Datacratic Inc.  All rights reserved.

   REST proxy class for http.
*/

#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/Info.hpp>
#include <curlpp/Infos.hpp>

#include "jml/arch/threads.h"
#include "jml/arch/timers.h"
#include "soa/types/basic_value_descriptions.h"

#include "http_rest_proxy.h"


using namespace std;
using namespace ML;


namespace Datacratic {


/*****************************************************************************/
/* HTTP REST PROXY                                                           */
/*****************************************************************************/

HttpRestProxy::
~HttpRestProxy()
{
    for (auto c: inactive)
        delete c;
}

HttpRestProxy::Response
HttpRestProxy::
perform(const std::string & verb,
        const std::string & resource,
        const Content & content,
        const RestParams & queryParams,
        const RestParams & headers,
        double timeout,
        bool exceptions,
        OnData onData) const
{
    string responseHeaders;
    string body;
    string uri;

    try {
        responseHeaders.clear();
        body.clear();

        Connection connection = getConnection();

        curlpp::Easy & myRequest = *connection;

        using namespace curlpp::options;
        using namespace curlpp::infos;
            
        list<string> curlHeaders;
        for (unsigned i = 0;  i < headers.size();  ++i) {
            curlHeaders.push_back(headers[i].first + ": "
                                  + headers[i].second);
        }

        uri = serviceUri + resource + queryParams.uriEscaped();

        //cerr << "uri = " << uri << endl;
        
        myRequest.setOpt<CustomRequest>(verb);

        myRequest.setOpt<curlpp::options::Url>(uri);

        if (debug)
            myRequest.setOpt<Verbose>(true);

        myRequest.setOpt<ErrorBuffer>((char *)0);
        if (timeout != -1)
            myRequest.setOpt<curlpp::OptionTrait<long, CURLOPT_TIMEOUT_MS> >(timeout * 1000);
        else myRequest.setOpt<Timeout>(0);
        myRequest.setOpt<NoSignal>(1);

        if (noSSLChecks) {
            myRequest.setOpt<SslVerifyHost>(false);
            myRequest.setOpt<SslVerifyPeer>(false);
        }

        // auto onData = [&] (char * data, size_t ofs1, size_t ofs2) -> size_t
        //     {
        //         //cerr << "called onData for " << ofs1 << " " << ofs2 << endl;
        //         return 0;
        //     };

        auto onWriteData = [&] (char * data, size_t ofs1, size_t ofs2) -> size_t
            {
                if (debug)
                    cerr << "got data " << string(data, data + ofs1 * ofs2) << endl;

                if (onData) {
                    if (!onData(string(data, data + ofs1 * ofs2)))
                        return 0;
                }

                body.append(data, ofs1 * ofs2);
                return ofs1 * ofs2;
                //cerr << "called onWrite for " << ofs1 << " " << ofs2 << endl;
                return 0;
            };

        auto onProgress = [&] (double p1, double p2, double p3, double p4) -> int
            {
                cerr << "progress " << p1 << " " << p2 << " " << p3 << " "
                << p4 << endl;
                return 0;
            };

        bool afterContinue = false;

        //cerr << endl << endl << "*******************" << endl;

        auto onHeader = [&] (char * data, size_t ofs1, size_t ofs2) -> size_t
            {
                string headerLine(data, ofs1 * ofs2);

                if (debug)
                    cerr << "got header " << headerLine << endl;

                if (headerLine.find("HTTP/1.1 100 Continue") == 0) {
                    afterContinue = true;
                }
                else if (afterContinue) {
                    if (headerLine == "\r\n")
                        afterContinue = false;
                }
                else {
                    responseHeaders.append(headerLine);
                    //cerr << "got header data " << headerLine << endl;
                }
                return ofs1 * ofs2;
            };

        myRequest.setOpt<BoostHeaderFunction>(onHeader);
        myRequest.setOpt<BoostWriteFunction>(onWriteData);
        myRequest.setOpt<BoostProgressFunction>(onProgress);
        for (auto & cookie: cookies)
            myRequest.setOpt<curlpp::options::CookieList>(cookie);

        //myRequest.setOpt<Header>(true);

        if (content.data) {
            string s(content.data, content.size);
            myRequest.setOpt<PostFields>(s);
            myRequest.setOpt<PostFieldSize>(content.size);
            curlHeaders.push_back(ML::format("Content-Length: %lld",
                                             content.size));
            //curlHeaders.push_back("Transfer-Encoding:");
            curlHeaders.push_back("Content-Type: " + content.contentType);
        }
        else {
            myRequest.setOpt<PostFieldSize>(-1);
            myRequest.setOpt<PostFields>("");
        }

        myRequest.setOpt<curlpp::options::HttpHeader>(curlHeaders);

        if (exceptions) {
            myRequest.perform();
        }
        else {
            CURLcode code = curl_easy_perform(myRequest.getHandle());
            if (code != CURLE_OK) {
                Response response;
                response.errorCode_ = code;
                response.errorMessage_ = curl_easy_strerror(code);
                return response;
            }
        }

        Response response;
        response.body_ = body;

        curlpp::InfoGetter::get(myRequest, CURLINFO_RESPONSE_CODE,
                                response.code_);

        double bytesUploaded;
    
        curlpp::InfoGetter::get(myRequest, CURLINFO_SIZE_UPLOAD,
                                bytesUploaded);

        //cerr << "uploaded " << bytesUploaded << " bytes" << endl;

        response.header_.parse(responseHeaders);

        return response;
    } catch (const curlpp::LibcurlRuntimeError & exc) {
        if (exc.whatCode() == CURLE_OPERATION_TIMEDOUT)
            throw;
        cerr << "libCurl returned an error with code " << exc.whatCode()
             << endl;
        cerr << "error is " << curl_easy_strerror(exc.whatCode())
             << endl;
        cerr << "verb is " << verb << endl;
        cerr << "uri is " << uri << endl;
        //cerr << "query params are " << queryParams << endl;
        cerr << "headers are " << responseHeaders << endl;
        cerr << "body contains " << body.size() << " bytes" << endl;
        throw;
    }
}

HttpRestProxy::Connection::
~Connection()
{
    if (!conn)
        return;
    proxy->doneConnection(conn);
}

HttpRestProxy::Connection
HttpRestProxy::
getConnection() const
{
    std::unique_lock<std::mutex> guard(lock);

    if (inactive.empty()) {
        return Connection(new curlpp::Easy, const_cast<HttpRestProxy *>(this));
    }
    else {
        auto res = inactive.back();
        inactive.pop_back();
        return Connection(res, const_cast<HttpRestProxy *>(this));
    }
}

void
HttpRestProxy::
doneConnection(curlpp::Easy * conn)
{
    std::unique_lock<std::mutex> guard(lock);
    inactive.push_back(conn);
}


/****************************************************************************/
/* JSON REST PROXY                                                          */
/****************************************************************************/

JsonRestProxy::
JsonRestProxy(const string & url)
    : HttpRestProxy(url), maxRetries(10)
{
    if (url.find("https://") == 0) {
        cerr << "warning: no validation will be performed on the SSL cert.\n";
        noSSLChecks = true;
    }
}

HttpRestProxy::Response
JsonRestProxy::
putOrPost(const string & resource, const string & body, bool isPost)
    const
{
    HttpRestProxy::Response response;
    const char * verb = isPost ? "POST" : "PUT";

    JML_TRACE_EXCEPTIONS(false);

    Content content(body, "application/json");
    RestParams headers;

    // cerr << "posting data to " + resource + "\n";
    if (authToken.size() > 0) {
        headers.emplace_back(make_pair("Cookie", "token=\"" + authToken + "\""));
    }

    string method = isPost ? "POST" : "PUT";
    pid_t tid = gettid();
    size_t retries;
    for (retries = 0; retries < maxRetries; retries++) {
        response = this->perform(method, resource, content, RestParams(),
                                 headers);
        int code = response.code();
        if (code < 400) {
            break;
        }

        /* error handling */
        if (retries == 0) {
            string respBody = response.body();
            ::fprintf(stderr,
                      "[%d] %s %s returned response code %d"
                      " (attempt %lu):\n"
                      "request body (%lu) = '%s'\n"
                      "response body (%lu): '%s'\n",
                      tid, verb, resource.c_str(), code, retries,
                      body.size(), body.c_str(),
                      respBody.size(), respBody.c_str());
        }
        if (code < 500) {
            throw ML::Exception("[%d] error is unrecoverable");
        }

        /* recoverable errors */
        if (retries < maxRetries) {
            sleepAfterRetry(retries);
            ::fprintf(stderr, "[%d] retrying %s %s after error"
                      " (%lu/%lu)\n",
                      tid, verb, resource.c_str(), retries + 1, maxRetries);
        }
        else {
            throw ML::Exception("[%d] too many retries\n", tid);
        }
    }

    return response;
}

HttpRestProxy::Response
JsonRestProxy::
get(const string & resource)
    const
{
    RestParams headers;

    if (authToken.size() > 0) {
        headers.emplace_back(make_pair("Cookie", "token=\"" + authToken + "\""));
    }

    return HttpRestProxy::get(resource, RestParams(), headers);
}

bool
JsonRestProxy::
authenticate(const JsonAuthenticationRequest & creds)
{
    bool result;

    try {
        auto authResponse = postTyped<JsonAuthenticationResponse>("/authenticate", creds, 200);
        authToken = authResponse.token;
        result = true;
    }
    catch (const ML::Exception & exc) {
        result = false;
    }

    return result;
}

void
JsonRestProxy::
sleepAfterRetry(int retryNbr)
{
    static const double sleepUnit(0.2);
    int rnd = random();

    int maxSlot = (1 << retryNbr) - 1;
    double timeToSleep = ((double) rnd / RAND_MAX) * maxSlot * sleepUnit;
    // cerr << "sleeping " << timeToSleep << endl;

    ML::sleep(timeToSleep);
}


/****************************************************************************/
/* JSON AUTHENTICATION REQUEST                                              */
/****************************************************************************/

JsonAuthenticationRequestDescription::
JsonAuthenticationRequestDescription()
{
    addField("email", &JsonAuthenticationRequest::email, "");
    addField("password", &JsonAuthenticationRequest::password, "");
}


/****************************************************************************/
/* JSON AUTHENTICATION RESPONSE                                             */
/****************************************************************************/

JsonAuthenticationResponseDescription::
JsonAuthenticationResponseDescription()
{
    addField("token", &JsonAuthenticationResponse::token, "");
}

} // namespace Datacratic
