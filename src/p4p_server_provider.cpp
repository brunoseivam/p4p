
#include <map>

#include <time.h>
#include <stddef.h>

#ifndef CLOCK_MONOTONIC_COARSE
#  ifdef CLOCK_MONOTONIC
#    define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#  else
#    define CLOCK_MONOTONIC_COARSE CLOCK_REALTIME
#  endif
#endif

#include <pv/serverContext.h>

#include "p4p.h"

namespace {

namespace pvd = epics::pvData;
namespace pva = epics::pvAccess;

struct PyServerChannel;

const unsigned maxCache = 100;
const unsigned expireIn = 5;

struct PyServerProvider :
        public pva::ChannelProviderFactory,
        public pva::ChannelFind,
        public pva::ChannelProvider,
        public std::tr1::enable_shared_from_this<PyServerProvider>
{
    POINTER_DEFINITIONS(PyServerProvider);

    virtual ~PyServerProvider() {
        TRACE("Being destroyed. provider obj="<<provider.ref.get()<<" refs="<<(provider.ref.get() ? Py_REFCNT(provider.ref.get()) : 0));
    }

    PyExternalRef provider;
    std::string provider_name;

    // cache negative search results (names we don't have)
    // map name -> expiration time
    typedef std::map<std::string, timespec> search_cache_t;
    search_cache_t search_cache;
    epicsMutex search_cache_lock;

    virtual std::string getFactoryName() { return provider_name; }
    virtual ChannelProvider::shared_pointer sharedInstance() {
        TRACE("GET");
        return shared_from_this();
    }
    virtual ChannelProvider::shared_pointer newInstance(const std::tr1::shared_ptr<pva::Configuration>&) {
        ChannelProvider::shared_pointer ret(shared_from_this());
        TRACE("GET "<<ret.use_count());
        return ret;
    }

    virtual std::tr1::shared_ptr<pva::ChannelProvider> getChannelProvider() { return shared_from_this(); }
    virtual void cancel() {}

    virtual void lock() {}
    virtual void unlock() {}
    virtual void destroy() {}

    virtual std::string getProviderName() { return provider_name; }

    virtual pva::ChannelFind::shared_pointer channelFind(std::string const & channelName,
            pva::ChannelFindRequester::shared_pointer const & channelFindRequester)
    {
        timespec now = {0,0};
        clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
        TRACE("ENTER "<<channelName);
        pva::ChannelFind::shared_pointer ret;
        try {
            // To reduce load we maintain a cache of failed name searches
            // which we don't repeat too often

            bool hit;
            {
                Guard G(search_cache_lock);
                search_cache_t::iterator it = search_cache.find(channelName);
                if(it!=search_cache.end() && it->second.tv_sec < now.tv_sec) {
                    // stale entry
                    search_cache.erase(it);
                    it = search_cache.end();
                }
                hit = it != search_cache.end();
            }

            if(hit) {
                TRACE("HIT "<<channelName);
                channelFindRequester->channelFindResult(pvd::Status::Ok,
                                                        ret, false);
                return ret;
            }

            bool found;
            {
                PyLock G;
    
                PyRef grab(PyObject_CallMethod(provider.ref.get(), "testChannel", "s", channelName.c_str()), allownull());
                if(!grab.get()) {
                    PyErr_Print();
                    PyErr_Clear();
                    found = false;

                } else if(PyObject_IsTrue(grab.get())) {
                    found = true;

                } else {
                    found = false;
                }
            }


            if(found) {
                ret = shared_from_this();
                channelFindRequester->channelFindResult(pvd::Status::Ok,
                                                        ret, true);

            } else {
                channelFindRequester->channelFindResult(pvd::Status::Ok,
                                                        ret, false);

                now.tv_sec+=expireIn;

                {
                    Guard G(search_cache_lock);
                    if(search_cache.size()>=maxCache) {
                        // instead of a proper LRU cache, just drop when it gets too big
                        TRACE("CLEAR");
                        search_cache.clear();
                    }
                    TRACE("ADD "<<channelName);
                    search_cache.insert(std::make_pair(channelName, now));
                }
            }
        } catch(std::exception& e) {
            channelFindRequester->channelFindResult(pvd::Status::Ok,
                                                    ret, false);
            std::cerr<<"Unhandled exception in channelFind(): "<<e.what()<<"\n";
        }
        TRACE("EXIT "<<(ret ? "Claim" : "Ignore"));
        return ret;
    }

    virtual pva::ChannelFind::shared_pointer channelList(pva::ChannelListRequester::shared_pointer const & channelListRequester)
    {
        pva::ChannelFind::shared_pointer ret;
        channelListRequester->channelListResult(pvd::Status(pvd::Status::STATUSTYPE_FATAL, "Not implemented"),
                                                ret,
                                                pvd::PVStringArray::const_svector(),
                                                false);
        return ret;
    }

    virtual pva::Channel::shared_pointer createChannel(std::string const & channelName,
                                                       pva::ChannelRequester::shared_pointer const & channelRequester,
                                                       short priority)
    {
        return createChannel(channelName, channelRequester, priority, "<unknown>");
    }

    virtual pva::Channel::shared_pointer createChannel(std::string const & channelName,
                                                       pva::ChannelRequester::shared_pointer const & channelRequester,
                                                       short priority, std::string const & address);
};

struct PyServerChannel :
        public pva::Channel,
        public std::tr1::enable_shared_from_this<PyServerChannel>
{
    POINTER_DEFINITIONS(PyServerChannel);

    PyServerProvider::shared_pointer provider;
    pva::ChannelRequester::shared_pointer requester;
    const std::string name;
    PyExternalRef handler;
    //! type description for get/put
    pvd::Structure::const_shared_pointer type;

    PyServerChannel(const PyServerProvider::shared_pointer& provider,
                    const pva::ChannelRequester::shared_pointer& req,
                    const std::string& name,
                    PyRef& py)
        :provider(provider)
        ,requester(req)
        ,name(name)
    {
        handler.swap(py);
    }
    virtual ~PyServerChannel() {TRACE("dtor provider refs="<<provider.use_count());}

    virtual void destroy() {}

    virtual std::tr1::shared_ptr<pva::ChannelProvider> getProvider() { return provider; }
    virtual std::string getRemoteAddress() { return requester->getRequesterName(); }
    virtual std::string getChannelName() { return name; }
    virtual std::tr1::shared_ptr<pva::ChannelRequester> getChannelRequester() { return requester; }

    virtual pva::ChannelRPC::shared_pointer createChannelRPC(
            pva::ChannelRPCRequester::shared_pointer const & channelRPCRequester,
            pvd::PVStructure::shared_pointer const & pvRequest);

//    virtual pva::ChannelGet::shared_pointer createChannelGet(
//            const pva::ChannelGetRequester::shared_pointer &channelGetRequester,
//            const pvd::PVStructure::shared_pointer &pvRequest);
};

// common base class for our operations
template<typename Base>
struct PyServerCommon : public Base
{
    typedef Base operation_type;
    typedef typename Base::requester_type requester_type;
    PyServerChannel::shared_pointer chan;
    typename requester_type::shared_pointer requester;
    pvd::PVStructure::shared_pointer pvRequest;

    PyServerCommon(const PyServerChannel::shared_pointer& c,
                   const pvd::PVStructure::shared_pointer& pvR,
                   const typename requester_type::shared_pointer& r) :chan(c), requester(r), pvRequest(pvR) {}
    virtual ~PyServerCommon() {}

    virtual void lock() {}
    virtual void unlock() {}
    virtual void destroy() {this->cancel();}

    virtual pva::Channel::shared_pointer getChannel() { return chan; }

    virtual void lastRequest() {}

};

struct PyServerRPC : public PyServerCommon<pva::ChannelRPC>,
                     public std::tr1::enable_shared_from_this<PyServerRPC>
{
    typedef PyServerCommon<pva::ChannelRPC> base_type;
    POINTER_DEFINITIONS(PyServerRPC);

    struct ReplyData {
        PyServerRPC::shared_pointer rpc;
        ReplyData() {}
        ~ReplyData() {
            if(rpc) {
                if(PyErr_Warn(PyExc_UserWarning, "RPCReply collected before reply")) {
                    PyErr_Print();
                    PyErr_Clear();
                }
                TRACE("rpc dropped");
                PyUnlock U;
                pvd::Status error(pvd::Status::STATUSTYPE_ERROR, "No Reply");
                rpc->requester->requestDone(error, rpc, pvd::PVStructure::shared_pointer());
            }
        }
    };
    static PyTypeObject *ReplyData_type;

    typedef PyClassWrapper<ReplyData> Reply;
    // active_reply should be cleared when Reply::rpc is cleared
    ReplyData* active_reply;

    PyServerRPC(const PyServerChannel::shared_pointer& c,
                const pvd::PVStructure::shared_pointer& pvR,
                const base_type::requester_type::shared_pointer& r) :base_type(c, pvR, r), active_reply(0) {}
    virtual ~PyServerRPC() {TRACE("dtor");}

    virtual void request(pvd::PVStructure::shared_pointer const & pvArgument)
    {
        TRACE("ENTER");
        bool createdReply = false;
        PyLock G;
        if(active_reply)
            throw std::logic_error("Request already in progress");
        try {

            PyRef wrapper(PyObject_GetAttrString(chan->handler.ref.get(), "Value"));
            if(!PyType_Check(wrapper.get()))
                throw std::runtime_error("handler.Value is not a Type");

            PyRef req(P4PValue_wrap((PyTypeObject*)wrapper.get(), pvArgument));

            PyRef args(PyTuple_New(0));

            PyRef rep(ReplyData_type->tp_new(ReplyData_type, args.get(), 0));

            active_reply = &Reply::unwrap(rep.get());
            active_reply->rpc = shared_from_this();
            createdReply = true;
            // from this point the Reply is responsible for calling requestDone()
            // if the call to .rpc() fails then the reply is sent when the Reply is destroyed
            // which means that an exception thrown by rpc() will only be printed to screen

            PyRef junk(PyObject_CallMethod(chan->handler.ref.get(), "rpc", "OO", rep.get(), req.get()));

            TRACE("SUCCESS");
        } catch(std::exception& e) {
            active_reply = NULL;
            if(PyErr_Occurred()) {
                PyErr_Print();
                PyErr_Clear();
            }
            if(!createdReply) {
                pva::ChannelRPCRequester::shared_pointer R(requester);
                PyUnlock U;
                R->requestDone(pvd::Status(pvd::Status::STATUSTYPE_ERROR, e.what()),
                               shared_from_this(),
                               pvd::PVStructurePtr());
            }
            TRACE("ERROR "<<(createdReply ? "DELEGATE" : "SENT"));
        }
    }

    virtual void cancel() {
        PyLock L;
        if(active_reply) {
            active_reply->rpc.reset();
            active_reply = NULL;
        }
        //TODO: notify in progress ops?
    }

    // python methods of the ReplyData class

    static PyObject* reply_done(PyObject *self, PyObject *args, PyObject *kwds)
    {
        PyObject *data = Py_None;
        const char *error = NULL;
        const char *names[] = {"reply", "error", NULL};
        if(!PyArg_ParseTupleAndKeywords(args, kwds, "|Oz", (char**)names,
                                        &data,
                                        &error))
            return NULL;

        TRACE("ENTER");
        Reply::reference_type SELF = Reply::unwrap(self);
        try {
            if(!SELF.rpc) {
                TRACE("Cancelled");
                Py_RETURN_NONE;

            }

            pva::ChannelRPCRequester::shared_pointer R(SELF.rpc->requester);

            if(data!=Py_None) {


                pvd::PVStructure::shared_pointer value;

                if(PyObject_TypeCheck(data, P4PValue_type)) {
                    value = P4PValue_unwrap(data);
                } else {
                    return PyErr_Format(PyExc_ValueError, "RPC results must be Value");
                }

                PyServerRPC::shared_pointer rpc;
                SELF.rpc->active_reply = NULL;
                SELF.rpc.swap(rpc);
                {
                    PyUnlock U;
                    R->requestDone(pvd::Status::Ok,
                                   rpc,
                                   value);
                }
                TRACE("SUCCESS");
            } else if(error) {
                PyServerRPC::shared_pointer rpc;
                SELF.rpc->active_reply = NULL;
                SELF.rpc.swap(rpc);
                {
                    PyUnlock U;
                    R->requestDone(pvd::Status(pvd::Status::STATUSTYPE_ERROR, error),
                                           rpc,
                                           pvd::PVStructurePtr());
                }
                TRACE("ERROR");
            } else {
                return PyErr_Format(PyExc_ValueError, "done() needs reply= or error=");
            }

            Py_RETURN_NONE;
        }CATCH()
        TRACE("FAIL");
        return NULL;
    }
};

} // namespace


template<>
PyTypeObject PyServerRPC::Reply::type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_p4p.RPCReply",
    sizeof(PyServerRPC::Reply),
};

namespace {

PyTypeObject *PyServerRPC::ReplyData_type = &PyServerRPC::Reply::type;

/*
struct PyServerGet : public PyServerCommon<pva::ChannelGet>,
                     public std::tr1::enable_shared_from_this<PyServerGet>
{
    typedef PyServerCommon<pva::ChannelGet> base_type;
    POINTER_DEFINITIONS(PyServerGet);

    struct ReplyData {
        PyServerGet::shared_pointer op;
        bool sent;
        ReplyData() :sent(false) {}
        ~ReplyData() {
            if(!sent) {
                TRACE("get dropped");
                PyUnlock U;
                pvd::Status error(pvd::Status::STATUSTYPE_ERROR, "Operation dropped by server");
                op->requester->getDone(error, op, pvd::PVStructure::shared_pointer(), pvd::BitSet::shared_pointer());
            }
        }
    };

    typedef PyClassWrapper<ReplyData> Reply;

    PyServerGet(const PyServerChannel::shared_pointer& c,
                const pvd::PVStructure::shared_pointer& pvR,
                const typename base_type::requester_type::shared_pointer& r) :base_type(c, pvR, r) {}
    virtual ~PyServerGet() {}

    pvd::Structure::const_shared_pointer type;

    virtual void get()
    {
        bool createdReply = false;
        PyLock L;
        try {
            PyRef args(PyTuple_New(0));
            PyRef rep(Reply::type.tp_new(&Reply::type, args.get(), 0));

            Reply::unwrap(rep.get()).op = shared_from_this();
            createdReply = true;

            PyRef junk(PyObject_CallMethod(chan->handler.ref.get(), "get", "O", rep.get()));

            TRACE("SUCCESS");
        } catch(std::exception& e) {
            if(PyErr_Occurred()) {
                PyErr_Print();
                PyErr_Clear();
            }
            if(!createdReply) {
                pva::ChannelGetRequester::shared_pointer R(requester);
                PyUnlock U;
                R->getDone(pvd::Status(pvd::Status::STATUSTYPE_ERROR, e.what()),
                           shared_from_this(),
                           pvd::PVStructurePtr(),
                           pvd::BitSetPtr());
            }
            TRACE("ERROR "<<(createdReply ? "DELEGATE" : "SENT"));
        }

    }

    static PyObject* reply_done(PyObject *self, PyObject *args, PyObject *kwds)
    {
        PyObject *data = Py_None;
        const char *error = NULL;
        const char *names[] = {"reply", "error", NULL};
        if(!PyArg_ParseTupleAndKeywords(args, kwds, "|Oz", (char**)names,
                                        &data,
                                        &error))
            return NULL;

        TRACE("ENTER");
        Reply::reference_type SELF = Reply::unwrap(self);
        try {
            pva::ChannelGetRequester::shared_pointer R(SELF.op->requester);

            if(!SELF.op->active) {
                // cancelled by client, no-op
            } else if(SELF.sent) {
                return PyErr_Format(PyExc_TypeError, "done() already called");

            } else if(data!=Py_None) {


                pvd::PVStructure::shared_pointer value;
                // TODO: use bitset from Value, need to copy?
                pvd::BitSet::shared_pointer vset(new pvd::BitSet(1));
                vset->set(0);

                if(PyObject_TypeCheck(data, P4PValue_type)) {
                    value = P4PValue_unwrap(data);
                } else {
                    return PyErr_Format(PyExc_ValueError, "RPC results must be Value");
                }

                if(SELF.op->type!=value->getStructure()) {
                    //TODO: oops?
                }

                SELF.sent = true;
                PyUnlock U;
                R->getDone(pvd::Status::Ok,
                            SELF.op,
                            value, vset);
            } else if(error) {
                SELF.sent = true;
                PyUnlock U;
                R->getDone(pvd::Status(pvd::Status::STATUSTYPE_ERROR, error),
                                       SELF.op,
                                       pvd::PVStructurePtr(),
                                       pvd::BitSetPtr());
            } else {
                return PyErr_Format(PyExc_ValueError, "done() needs reply= or error=");
            }

            TRACE("SUCCESS");
            Py_RETURN_NONE;
        }CATCH()
        TRACE("ERROR");
        return NULL;
    }
};
*/

pva::Channel::shared_pointer
PyServerProvider::createChannel(std::string const & channelName,
                                                   pva::ChannelRequester::shared_pointer const & channelRequester,
                                                   short priority, std::string const & address)
{
    TRACE("ENTER "<<channelRequester->getRequesterName());
    pva::Channel::shared_pointer ret;
    try {
        PyLock G;

        PyRef handler(PyObject_CallMethod(provider.ref.get(), "makeChannel", "ss", channelName.c_str(),
                                      channelRequester->getRequesterName().c_str()), allownull());
        if(!handler.get()) {
            PyErr_Print();
            PyErr_Clear();
            channelRequester->channelCreated(pvd::Status(pvd::Status::STATUSTYPE_ERROR, "Logic Error"),
                                             ret);

        } else if(handler.get()==Py_None) {
            channelRequester->channelCreated(pvd::Status(pvd::Status::STATUSTYPE_ERROR, "No such channel"),
                                             ret);

        } else {
            ret.reset(new PyServerChannel(shared_from_this(),channelRequester, channelName, handler));
            // handler consumed now
            channelRequester->channelCreated(pvd::Status::Ok, ret);
        }
    } catch(std::exception& e) {
        channelRequester->channelCreated(pvd::Status(pvd::Status::STATUSTYPE_ERROR, e.what()),
                                         ret);
    }
    TRACE("EXIT "<<(ret ? "Create" : "Refuse"));
    return ret;
}

pva::ChannelRPC::shared_pointer
PyServerChannel::createChannelRPC(
        pva::ChannelRPCRequester::shared_pointer const & channelRPCRequester,
        pvd::PVStructure::shared_pointer const & pvRequest)
{
    TRACE("ENTER");
    pva::ChannelRPC::shared_pointer ret(new PyServerRPC(shared_from_this(), pvRequest, channelRPCRequester));
    channelRPCRequester->channelRPCConnect(pvd::Status::Ok, ret);
    return ret;
}
/*
pva::ChannelGet::shared_pointer
PyServerChannel::createChannelGet(
        const pva::ChannelGetRequester::shared_pointer &channelGetRequester,
        const pvd::PVStructure::shared_pointer &pvRequest)
{
    TRACE("ENTER");
    pvd::Structure::const_shared_pointer gtype(getType());
    pva::ChannelGet::shared_pointer ret(new PyServerGet(shared_from_this(), pvRequest, channelGetRequester));
    if(gtype)
        channelGetRequester->channelGetConnect(pvd::Status::Ok, ret);
    return ret;
}
*/

static struct PyMethodDef PyServerRPC_methods[] = {
    {"done", (PyCFunction)PyServerRPC::reply_done, METH_VARARGS|METH_KEYWORDS,
     "done(reply=Value)\n"
     "done(error=\"oops\")\n"
     "Complete RPC call with reply data or error message"},
    {NULL}
};

/*
static struct PyMethodDef PyServerGet_methods[] = {
    {"done", (PyCFunction)PyServerGet::reply_done, METH_VARARGS|METH_KEYWORDS,
     "done(reply=Value)\n"
     "done(error=\"oops\")\n"
     "Complete Get call with reply data or error message"},
    {NULL}
};
*/

typedef std::map<std::string, PyServerProvider::shared_pointer> pyproviders_t;
pyproviders_t* pyproviders;

} // namespace

PyObject* p4p_add_provider(PyObject *junk, PyObject *args, PyObject *kwds)
{
    const char *name;
    PyObject *prov;

    const char *names[] = {"name", "provider", NULL};
    if(!PyArg_ParseTupleAndKeywords(args, kwds, "sO", (char**)names, &name, &prov))
        return NULL;

    try {
        if(!pyproviders)
            pyproviders = new pyproviders_t;

        pyproviders_t::const_iterator it = pyproviders->find(name);
        if(it!=pyproviders->end())
            return PyErr_Format(PyExc_KeyError, "Provider %s already registered", name);

        PyRef handler(prov, borrow());
        PyServerProvider::shared_pointer P(new PyServerProvider);
        P->provider.swap(handler);
        P->provider_name = name;

        pva::ChannelProviderRegistry::servers()->add(P);

        (*pyproviders)[name] = P;
        TRACE("name="<<name<<" "<<P.use_count());

        Py_RETURN_NONE;
    }CATCH()
    return NULL;
}

PyObject* p4p_remove_provider(PyObject *junk, PyObject *args, PyObject *kwds)
{
    const char *name;
    const char *names[] = {"name", NULL};
    if(!PyArg_ParseTupleAndKeywords(args, kwds, "s", (char**)names, &name))
        return NULL;

    try {
        TRACE("Clear "<<name);
        if(!pyproviders)
            Py_RETURN_FALSE;

        pyproviders_t::iterator it = pyproviders->find(name);
        if(it==pyproviders->end())
            Py_RETURN_FALSE;

        pva::ChannelProviderRegistry::servers()->remove(it->second);

        TRACE("name="<<name<<" "<<it->second.use_count());
        pyproviders->erase(it);

        Py_RETURN_TRUE;
    }CATCH()
    return NULL;
}

PyObject* p4p_remove_all(PyObject *junk, PyObject *args, PyObject *kwds)
{
    const char *names[] = {NULL};
    if(!PyArg_ParseTupleAndKeywords(args, kwds, "", (char**)names))
        return NULL;

    try {
        TRACE("Clear");
        if(pyproviders) {
            std::auto_ptr<pyproviders_t> P(pyproviders);
            pyproviders = NULL;
            for(pyproviders_t::const_iterator it = P->begin(); it!=P->end(); ++it) {
                pva::ChannelProviderRegistry::servers()->remove(it->second);
            }
        }

        Py_RETURN_NONE;
    }CATCH()
    return NULL;
}

/*

template<>
PyTypeObject PyServerGet::Reply::type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_p4p.GetReply",
    sizeof(PyServerGet::Reply),
};
*/
void p4p_server_provider_register(PyObject *mod)
{
    pyproviders = NULL;

    PyServerRPC::Reply::buildType();
    PyServerRPC::Reply::type.tp_flags = Py_TPFLAGS_DEFAULT;
    // No init.  This type can't be created by python code

    PyServerRPC::Reply::type.tp_methods = PyServerRPC_methods;

    if(PyType_Ready(&PyServerRPC::Reply::type))
        throw std::runtime_error("failed to initialize RPCReply");

    Py_INCREF((PyObject*)&PyServerRPC::Reply::type);
    if(PyModule_AddObject(mod, "RPCReply", (PyObject*)&PyServerRPC::Reply::type)) {
        Py_DECREF((PyObject*)&PyServerRPC::Reply::type);
        throw std::runtime_error("failed to add _p4p.RPCReply");
    }

/*
    PyServerGet::Reply::buildType();
    PyServerGet::Reply::type.tp_flags = Py_TPFLAGS_DEFAULT;
    // No init.  This type can't be created by python code

    PyServerGet::Reply::type.tp_methods = PyServerGet_methods;

    if(PyType_Ready(&PyServerGet::Reply::type))
        throw std::runtime_error("failed to initialize GetReply");

    Py_INCREF((PyObject*)&PyServerGet::Reply::type);
    if(PyModule_AddObject(mod, "GetReply", (PyObject*)&PyServerGet::Reply::type)) {
        Py_DECREF((PyObject*)&PyServerGet::Reply::type);
        throw std::runtime_error("failed to add _p4p.GetReply");
    }
*/
}
