/*
 * This file is protected by Copyright. Please refer to the COPYRIGHT file 
 * distributed with this source distribution.
 * 
 * This file is part of REDHAWK core.
 * 
 * REDHAWK core is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU Lesser General Public License as published by the 
 * Free Software Foundation, either version 3 of the License, or (at your 
 * option) any later version.
 * 
 * REDHAWK core is distributed in the hope that it will be useful, but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License 
 * for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License 
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include <ossie/debug.h>
#include <ossie/ossieSupport.h>
#include <ossie/DeviceManagerConfiguration.h>
#include <ossie/CorbaUtils.h>
#include <ossie/ComponentDescriptor.h>
#include <ossie/FileStream.h>
#include <ossie/prop_utils.h>
#include <ossie/logging/loghelpers.h>
#include <ossie/EventChannelSupport.h>
#include <ossie/affinity.h>
#include "spdSupport.h"
#include "DeviceManager_impl.h"
#include "rh_logger_stdout.h"

namespace fs = boost::filesystem;

using namespace ossie;

rh_logger::LoggerPtr DeviceManager_impl::__logger;

DeviceManager_impl::DeviceManager_impl(
        const char*     DCDInput, 
        const char*     _rootfs, 
        const char*     _cachepath, 
        const char*     _logconfig_uri, 
        const struct utsname &uname, 
        bool           useLogCfgResolver,
        const char     *cpuBlackList,
        bool*          internalShutdown) :
    _registeredDevices(),
    devmgr_info(0)
{

    __logger = rh_logger::Logger::getResourceLogger("DeviceManager_impl");

    // These should probably be execparams at some point
    _fsroot                     = _rootfs;
    _cacheroot                  = _cachepath;
    _deviceConfigurationProfile = DCDInput;
    _uname                      = uname;
    _internalShutdown           = internalShutdown;
    _useLogConfigUriResolver    = useLogCfgResolver;

    // save  os and processor when matching deployments
    processor_name = _uname.machine;
    os_name = _uname.sysname;

    // resolve local sdr root
    fs::path tsdr = fs::path(_rootfs);
    if ( tsdr.has_parent_path() ) {
        _local_sdrroot  = tsdr.parent_path().string();
    }
    else {
        _local_sdrroot = getenv("SDRROOT");
    }
    _local_domroot = _local_sdrroot + "/dom";



    logging_config_prop = (StringProperty*)addProperty(logging_config_uri, 
                                                       "LOGGING_CONFIG_URI", 
                                                       "LOGGING_CONFIG_URI",
                                                       "readonly", 
                                                       "", 
                                                       "external", 
                                                       "configure");
    if (_logconfig_uri) {
        logging_config_prop->setValue(_logconfig_uri);
    }

    addProperty( _domainName,
                 "DOMAIN_NAME",
                 "DOMAIN_NAME",
                 "readonly",
                 "",
                 "external",
                 "property");

    addProperty( _deviceConfigurationProfile,
                 "DCD_FILE",
                 "DCD_FILE",
                 "readonly",
                 "",
                 "external",
                 "property");

    addProperty( _cacheroot,
                 "SDRCACHE",
                 "SDRCACHE",
                 "readonly",
                 "",
                 "external",
                 "property");

    addProperty(HOSTNAME,
               "HOSTNAME",
               "HOSTNAME",
               "readonly",
               "",
               "external",
               "property");

    addProperty(DEVICE_FORCE_QUIT_TIME,
               "DEVICE_FORCE_QUIT_TIME",
               "DEVICE_FORCE_QUIT_TIME",
               "readwrite",
               "",
               "external",
               "property");

    addProperty(CLIENT_WAIT_TIME,
                10000,
               "CLIENT_WAIT_TIME",
               "CLIENT_WAIT_TIME",
               "readwrite",
               "millisec",
               "external",
               "property");
 
    // translate cpuBlackList to cpu ids 
    try {
      cpu_blacklist = redhawk::affinity::get_cpu_list("cpu", cpuBlackList );
    }
    catch(...){
      std::cerr << " Error processing cpu blacklist for this manager." << std::endl;
    }

    // this is hard-coded here because 1.10 and earlier Device Managers do not
    //  have this property in their prf
    this->DEVICE_FORCE_QUIT_TIME = 0.5;

    char _hostname[1024];
    gethostname(_hostname, 1024);
    std::string hostname(_hostname);
    HOSTNAME = hostname;
    this->_dmnMgr = CF::DomainManager::_nil();
}


DeviceManager_impl::~DeviceManager_impl ()
{
    for (DeploymentList::iterator deployed = deployed_comps.begin(); deployed != deployed_comps.end(); ++deployed) {
        delete (deployed->second);
    }
    deployed_comps.clear();
}

void DeviceManager_impl::abort() {
    killPendingDevices(SIGKILL, 0);
    shutdown();
}

void DeviceManager_impl::killPendingDevices (int signal, int timeout) {
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

    for (DeviceList::iterator device = _pendingDevices.begin(); device != _pendingDevices.end(); ++device) {
        pid_t devicePid = (*device)->pid;
        kill(devicePid, signal);
    }

    // Wait for the remaining devices to exit
    if (timeout > 0) {
        boost::system_time end = boost::get_system_time() + boost::posix_time::microseconds(timeout);
        while (!_pendingDevices.empty()) {
            if (!pendingDevicesEmpty.timed_wait(lock, end)) {
                break;
            }
        }
    }
}


void DeviceManager_impl::parseDeviceConfigurationProfile(const char *overrideDomainName){

    LOG_TRACE(DeviceManager_impl, "Using DCD profile " << _deviceConfigurationProfile);
    try {
        _fileSys->exists(_deviceConfigurationProfile.c_str());
    } catch( CF::InvalidFileName& _ex ) {
        std::ostringstream emsg;
        emsg <<"Terminating device manager; DCD file " << _deviceConfigurationProfile << " does not exist; " << _ex.msg;
        LOG_TRACE(DeviceManager_impl, emsg.str());
      throw std::runtime_error(emsg.str().c_str());
    } catch ( std::exception& ex ) {
        std::ostringstream emsg;
        emsg << "The following standard exception occurred: "<<ex.what()<<" while checking if " << _deviceConfigurationProfile << " exists";
        LOG_TRACE(DeviceManager_impl, emsg.str());
        throw std::runtime_error(emsg.str().c_str());
    } catch ( CORBA::Exception& ex ) {
        std::ostringstream emsg;
        emsg << "The following CORBA exception occurred: "<<ex._name()<<" while checking if " << _deviceConfigurationProfile << " exists";
        LOG_TRACE(DeviceManager_impl, emsg.str());
        throw std::runtime_error(emsg.str().c_str());
    } catch( ... ) {
        std::ostringstream emsg;
	emsg << "Terminating device manager; unknown exception checking if " << _deviceConfigurationProfile << " exists; ";
        LOG_TRACE(DeviceManager_impl, emsg.str());
        throw std::runtime_error(emsg.str().c_str());
    }

    LOG_TRACE(DeviceManager_impl, "Parsing DCD profile")
    try {
        File_stream _dcd(_fileSys, _deviceConfigurationProfile.c_str());
        node_dcd.load(_dcd);
        _dcd.close();
    } catch ( ossie::parser_error& e ) {
        std::string parser_error_line = ossie::retrieveParserErrorLineNumber(e.what());
        std::ostringstream eout;
        eout << "Exiting device manager; failure parsing DCD: " << _deviceConfigurationProfile 
             << ". " << parser_error_line << " The XML parser returned the following error: " << e.what();
        LOG_TRACE(DeviceManager_impl, eout.str());
        throw std::runtime_error(e.what());
    } catch ( std::exception& ex ) {
        std::ostringstream eout;
        eout << "The following standard exception occurred: "<<ex.what()<<", while parsing the DCD: " << _deviceConfigurationProfile;
        LOG_TRACE(DeviceManager_impl, eout.str())
        throw std::runtime_error(eout.str().c_str());
    } catch ( CORBA::Exception& ex ) {
        std::ostringstream eout;
        eout << "The following CORBA exception occurred: "<<ex._name()<<", while parsing the DCD: " << _deviceConfigurationProfile;
        LOG_TRACE(DeviceManager_impl, eout.str())
        throw std::runtime_error(eout.str().c_str());
    } catch ( ... ) {
        LOG_TRACE(DeviceManager_impl, "Exiting device manager; Unexpected failure parsing DCD: " << _deviceConfigurationProfile );
        throw std::runtime_error("unexpected error");
    }

    _identifier = node_dcd.getID();
    _label = node_dcd.getName();
    LOG_TRACE(DeviceManager_impl, "DeviceManager id: " << node_dcd.getID() << " name: " << node_dcd.getName());

    if (overrideDomainName == NULL) {
        LOG_TRACE(DeviceManager_impl, "Reading domainname from DCD file")
        CORBA::String_var tmp_domainManagerName = node_dcd.getDomainManagerName();
        _domainManagerName = (char *)tmp_domainManagerName;
        _domainName = _domainManagerName.substr(0, _domainManagerName.find_first_of("/"));
    } else {
        LOG_TRACE(DeviceManager_impl, "Overriding domainname from DCD file")
        _domainName = overrideDomainName;
        _domainManagerName = _domainName + "/" + _domainName;
    }
}

/*
 * Populate DeviceManager's SPD
 *
 * Handle any exceptions associated with loading the SPD to the 
 * devmgrspdparser.
 */
void DeviceManager_impl::parseSpd() {

    std::string devmgrsoftpkg = node_dcd.getDeviceManagerSoftPkg();

    if (devmgrsoftpkg[0] != '/') {
        std::string dcdPath = _deviceConfigurationProfile.substr(0, _deviceConfigurationProfile.find_last_of('/'));
        devmgrsoftpkg = dcdPath + '/' + devmgrsoftpkg;
    }

    try {
      devmgr_info = local_spd::ProgramProfile::LoadProfile( _fileSys, devmgrsoftpkg.c_str(), _local_dom_filesys );
    }
    catch( std::runtime_error &ex ) {
        LOG_TRACE(DeviceManager_impl, ex.what() );
        throw;
    }
        
}



void DeviceManager_impl::setupImplementationForHost() {

    std::string _PROC_OS_PROPS( "<?xml version=\"1.0\" encoding=\"UTF-8\"?> \
<!DOCTYPE properties PUBLIC \"-//JTRS//DTD SCA V2.2.2 PRF//EN\" \"properties.dtd\"> \
<properties> \
<simple id=\"DCE:fefb9c66-d14a-438d-ad59-2cfd1adb272b\" mode=\"readonly\" name=\"processor_name\" type=\"string\"> \
    <description>SCA required property describing the CPU type</description> \
    <kind kindtype=\"property\"/> \
    <kind kindtype=\"allocation\"/> \
    <action type=\"eq\"/> \
  </simple>  \
  <simple id=\"DCE:4a23ad60-0b25-4121-a630-68803a498f75\" mode=\"readonly\" name=\"os_name\" type=\"string\"> \
    <description>SCA required property describing the Operating System Name</description> \
    <kind kindtype=\"property\"/> \
    <kind kindtype=\"allocation\"/> \
    <action type=\"eq\"/> \
  </simple> \
</properties> \
 ");

    // create property set for os and processor matching
    std::istringstream prfdata(_PROC_OS_PROPS);
    host_props.load(prfdata);

    ossie::ComponentPropertyList _my_host;
    SimplePropertyRef  pref;
    pref._id = "DCE:fefb9c66-d14a-438d-ad59-2cfd1adb272b";
    pref._value = processor_name;
    _my_host.push_back( pref.clone() );
    LOG_INFO(DeviceManager_impl, "adding in property for :" << pref._id << " value : " << processor_name );
    pref._id = "DCE:4a23ad60-0b25-4121-a630-68803a498f75";
    pref._value = os_name;
    _my_host.push_back( pref.clone() );
    LOG_INFO(DeviceManager_impl, "adding in property for :" << pref._id << " value : " << os_name );
    host_props.override(_my_host);

    local_spd::ImplementationInfo::List  _allDevManImpls;
    devmgr_info->getImplementations(_allDevManImpls);
    local_spd::ImplementationInfo::List::const_iterator itr;
    if (_allDevManImpls.size() == 0) {
      std::string emsg="Device manager SPD has no implementations to match against.";
      LOG_ERROR(DeviceManager_impl, emsg);
      throw std::runtime_error(emsg);
    }

    bool found_impl=false;
    for (itr = _allDevManImpls.begin(); itr != _allDevManImpls.end(); ++itr) {
        if ( (*itr)->checkProcessorAndOs( host_props ) ) {
            devmgr_info->setSelectedImplementation(*itr);
	    devmgr_info->prf.join(host_props);
	    found_impl=true;
            break;
        }
    }

    if (!found_impl){
        std::ostringstream oss;
        oss << "Unable to find device manager implementation to match processor: " << _uname.machine;
        LOG_TRACE(DeviceManager_impl, oss.str() );
        throw std::runtime_error( oss.str().c_str());
    }
    LOG_TRACE(DeviceManager_impl, "Using device manager implementation " << devmgr_info->getID());
}

void DeviceManager_impl::resolveNamingContext(){
    base_context = ossie::corba::stringToName(_domainName);
    bool warnedMissing = false;
    while (true) {
        try {
            CORBA::Object_var obj = ossie::corba::InitialNamingContext()->resolve(base_context);
            rootContext = CosNaming::NamingContext::_narrow(obj);
            LOG_TRACE(DeviceManager_impl, "Connected");
            break;
        } catch ( ... ) {
            if (!warnedMissing) {
                warnedMissing = true;
                LOG_WARN(DeviceManager_impl, "Unable to find naming context " << _domainManagerName << "; retrying");
            }
        }
        // Sleep for a tenth of a second to give the DomainManager a chance to
        // create its naming context.
        usleep(10000);

        // If a shutdown occurs while waiting, turn it into an exception.
        if (*_internalShutdown) {
            LOG_TRACE(DeviceManager_impl, "Interrupted when waiting to locate DomainManager naming context");
            throw std::runtime_error("Interrupted when waiting to locate DomainManager naming context");
        }
    }
    LOG_TRACE(DeviceManager_impl, "Resolved DomainManager naming context");
}

/*
 * Record the mapping of the component instantiation id to the matched 
 * implementation id.  The scope is needed to remain consistent with the 
 * scoped lock protection for the map.
 */
void DeviceManager_impl::recordComponentInstantiationId(
        const ComponentInstantiation& instantiation,
        const std::string &impl_id ) {

    boost::mutex::scoped_try_lock lock(componentImplMapmutex);
    _componentImplMap[instantiation.getID()] = impl_id;
}

bool DeviceManager_impl::getCodeFilePath(
        std::string&                codeFilePath,
        const local_spd::ImplementationInfo&   matchedDeviceImpl,
        SoftPkg&                    SPDParser,
        FileSystem_impl*&           fs_servant,
	bool                        useLocalFileSystem ) {

    LOG_TRACE(DeviceManager_impl, "getCodeFile:  spdPath: " << SPDParser.getSPDPath() );
    LOG_TRACE(DeviceManager_impl, "getCodeFile:  localFileName: " << matchedDeviceImpl.getLocalFileName());
    LOG_TRACE(DeviceManager_impl, "getCodeFile:  entryPoint: " <<  matchedDeviceImpl.getEntryPoint());

    // get code file (the path to the device that must be run)
    fs::path codeFile = fs::path(matchedDeviceImpl.getLocalFileName());
    if (!codeFile.has_root_directory()) {
        codeFile = fs::path(SPDParser.getSPDPath()) / codeFile;
        LOG_TRACE(DeviceManager_impl, "code localfile had relative path; absolute path: " << codeFile);
    }
    codeFile = codeFile.normalize();

    fs::path entryPoint;
    if (matchedDeviceImpl.getEntryPoint().size() != 0) {
        LOG_TRACE(DeviceManager_impl, "Using provided entry point: " << matchedDeviceImpl.getEntryPoint())
        entryPoint = fs::path(matchedDeviceImpl.getEntryPoint());
        if (!entryPoint.has_root_directory()) {
            entryPoint = fs::path(SPDParser.getSPDPath()) / entryPoint;
            LOG_TRACE(DeviceManager_impl, "code entrypoint had relative path; absolute path: " << entryPoint);
        }
        entryPoint = entryPoint.normalize();
    } else if (matchedDeviceImpl.getEntryPoint().size() == 0) {
        LOG_ERROR(DeviceManager_impl, "not instantiating device; no entry point provided");
        return false;
    }

    std::string localFilePath = fs_servant->getLocalPath(entryPoint.string().c_str());
    if (useLocalFileSystem ) {
      codeFilePath = fs_servant->getLocalPath(entryPoint.string().c_str());
    }
    else {
      codeFilePath = entryPoint.string().c_str();
    }

    if (codeFilePath.length() == 0) {
        LOG_WARN(DeviceManager_impl, "Invalid device file. Could not find executable for " << codeFile)
        return false;
    }

    if (access(localFilePath.c_str(), F_OK) == -1) {
        std::string errMsg = "Unable to access local filesystem file: " + localFilePath;
        LOG_ERROR(DeviceManager_impl, errMsg );
        return false;
    }

    LOG_TRACE(DeviceManager_impl, "Code file path: " << codeFilePath)

    return true;
}

/*
 * Call rootContext->bind_new_context and handle any exceptions.
 */
void DeviceManager_impl::bindNamingContext() {
    CosNaming::Name devMgrContextName;
    devMgrContextName.length(1);
    devMgrContextName[0].id = CORBA::string_dup(_label.c_str());
    try {
        devMgrContext = rootContext->bind_new_context(devMgrContextName);
    } catch (CosNaming::NamingContext::AlreadyBound&) {
        LOG_WARN(DeviceManager_impl, "Device manager name already bound")
        rootContext->unbind(devMgrContextName);
        devMgrContext = rootContext->bind_new_context(devMgrContextName);
    } catch ( std::exception& ex ) {
        std::ostringstream eout;
        eout << "The following standard exception occurred: "<<ex.what()<<" while creating the Device Manager naming context";
        LOG_FATAL(DeviceManager_impl, eout.str())
        throw std::runtime_error(eout.str().c_str());
    } catch ( CORBA::Exception& ex ) {
        std::ostringstream eout;
        eout << "The following CORBA exception occurred: "<<ex._name()<<" while creating the Device Manager naming context";
        LOG_FATAL(DeviceManager_impl, eout.str())
        throw std::runtime_error(eout.str().c_str());
    } catch( ... ) {
        LOG_FATAL(DeviceManager_impl, "Unable to create device manager context")
        throw std::runtime_error("unexpected error");
    }
}


/*
 * Populates _domainManagerName by calling getDomainManagerReference.
 *
 * If an exception is thrown by getDomainManagerReference, this method will 
 * catch it, log ean error, and rethrow the exception.
 */
void DeviceManager_impl::getDomainManagerReferenceAndCheckExceptions() {

    LOG_INFO(DeviceManager_impl, "Connecting to Domain Manager " << _domainManagerName)
    try {
        getDomainManagerReference(_domainManagerName);
    } catch ( std::exception& ex ) {
        std::ostringstream eout;
        eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to reach the Domain Manager";
        LOG_FATAL(DeviceManager_impl, eout.str())
        throw std::runtime_error(eout.str().c_str());
    } catch ( CORBA::Exception& ex ) {
        std::ostringstream eout;
        eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to reach the Domain Manager";
        LOG_FATAL(DeviceManager_impl, eout.str())
        throw std::runtime_error(eout.str().c_str());
    } catch ( ... ) {
        LOG_FATAL(DeviceManager_impl, "[DeviceManager::post_constructor] Unable to get a reference to the DomainManager");
        throw std::runtime_error("unexpected error");
    }

    if (CORBA::is_nil(_dmnMgr)) {
        LOG_FATAL(DeviceManager_impl, "Failure getting Domain Manager")
        throw std::runtime_error("unexpected error");
    }
}

void DeviceManager_impl::registerDeviceManagerWithDomainManager(
        CF::DeviceManager_var& my_object_var) {

    LOG_TRACE(DeviceManager_impl, "Registering with DomainManager");
    int64_t cnt=0;
    while (true) {
        if (*_internalShutdown) {
            throw std::runtime_error("Interrupted waiting to register with DomainManager");
        }
        try {
            cnt++;
            _dmnMgr->registerDeviceManager(my_object_var);
            break;
        } catch (const CORBA::TRANSIENT& ex) {
            // The DomainManager isn't currently reachable, but it may become accessible again.
          if ( !(++cnt % 10) ) {LOG_WARN(DeviceManager_impl, "DomainManager not available,  TRANSIENT condition: retry cnt" << cnt); }
            usleep(100000);
        } catch (const CORBA::OBJECT_NOT_EXIST& ex) {
            // This error occurs while the DomainManager is still being constructed 
          if ( !(++cnt % 10) ) {LOG_WARN(DeviceManager_impl, "DomainManager not available,  DOES NOT EXIST condition: retry cnt" << cnt); }
            usleep(100000);
        } catch (const CF::DomainManager::RegisterError& e) {
            LOG_ERROR(DeviceManager_impl, "Failed to register with domain manager due to: " << e.msg);
            throw std::runtime_error("Error registering with Domain Manager");
        } catch (const CF::InvalidObjectReference& _ex) {
            LOG_FATAL(DeviceManager_impl, "While registering DevMgr with DomMgr: " << _ex.msg);
            throw std::runtime_error("Error registering with Domain Manager");
        } catch ( std::exception& ex ) {
            std::ostringstream eout;
            eout << "The following standard exception occurred: "<<ex.what()<<" while registering the Device Manager with the Domain Manager";
            LOG_FATAL(DeviceManager_impl, eout.str())
            throw std::runtime_error(eout.str().c_str());
        } catch ( CORBA::Exception& ex ) {
            std::ostringstream eout;
            eout << "The following CORBA exception occurred: "<<ex._name()<<" while registering the Device Manager with the Domain Manager";
            LOG_FATAL(DeviceManager_impl, eout.str())
            throw std::runtime_error(eout.str().c_str());
        } catch (...) {
            LOG_FATAL(DeviceManager_impl, "While registering DevMgr with DomMgr: Unknown Exception");
            throw std::runtime_error("Error registering with Domain Manager");
        }
    }
}

void DeviceManager_impl::getCompositeDeviceIOR(
        std::string&                                  compositeDeviceIOR, 
        const std::vector<ossie::ComponentPlacement>& componentPlacements,
        const ossie::ComponentPlacement&              componentPlacementInst) {

    //see if component is composite part of device
    LOG_TRACE(DeviceManager_impl, "Checking composite part of device");
    if (componentPlacementInst.isCompositePartOf()) {
        std::string parentDeviceRefid = componentPlacementInst.getCompositePartOfDeviceID();
        LOG_TRACE(DeviceManager_impl, "CompositePartOfDevice: <" << parentDeviceRefid << ">");
        //find parent ID and stringify the IOR
        for (unsigned int cp_idx = 0; cp_idx < componentPlacements.size(); cp_idx++) {
            // must match to a particular instance
            for (unsigned int ci_idx = 0; ci_idx < componentPlacements[cp_idx].getInstantiations().size(); ci_idx++) {
                const char* instanceID = componentPlacements[cp_idx].instantiations[ci_idx].getID();
                if (strcmp(instanceID, parentDeviceRefid.c_str()) == 0) {
                    LOG_TRACE(DeviceManager_impl, "CompositePartOfDevice: Found parent device instance <" 
                            << componentPlacements[cp_idx].getInstantiations()[ci_idx].getID() 
                            << "> for child device <" << componentPlacementInst.getFileRefId() << ">");
                    // now get the associated IOR
                    while (true) {
                        std::string tmpior = getIORfromID(instanceID);
                        if (!tmpior.empty()) {
                            compositeDeviceIOR = tmpior;
                            LOG_TRACE(DeviceManager_impl, "CompositePartOfDevice: Found parent device IOR <" << compositeDeviceIOR << ">");
                            break;
                        }
                        usleep(100);
                    }
                }

            }
        }
    }
}

CF::Properties DeviceManager_impl::getResourceOptions( const ossie::ComponentInstantiation& instantiation ){

  CF::Properties   options;
  CF::Properties   affinity_options;
  const ossie::ComponentInstantiation::AffinityProperties c_props = instantiation.getAffinity();
  if ( c_props.size() > 0  ){
    LOG_DEBUG(DeviceManager_impl, "Converting AFFINITY properties, resource: " << instantiation.getUsageName());
    ossie::convertComponentProperties( instantiation.getAffinity(), affinity_options );
    // Pass all afinity settings under single options list
    for ( uint32_t i=0; i < affinity_options.length(); i++ ) {
      CF::DataType dt = affinity_options[i];
      LOG_DEBUG(DeviceManager_impl, "Found Affinity Property: directive id:"  <<  dt.id << "/" <<  ossie::any_to_string( dt.value )) ;
    }
  }

  // add affinity properties as subtree to a resource option's property set
  if ( affinity_options.length() > 0 ) {
    options.length(options.length()+1);
    options[options.length()-1].id = CORBA::string_dup("AFFINITY"); 
    options[options.length()-1].value <<= affinity_options;
    LOG_DEBUG(DeviceManager_impl,"Extending Options property set with Affinity properties, nprops: " << affinity_options.length());
  }
  return options;
}


/* 
 * Get the type, which should be either "device" or 
 * "service" ("executabledevice" and "loadabledevice" are 
 * considered "device"s).  If the type is neither "device" nor
 * service, log an error.
 */
bool DeviceManager_impl::getDeviceOrService(
        std::string& type, 
	const local_spd::ProgramProfile *comp ) {

    bool supported = false;

    type = comp->scd.getComponentType();
    LOG_TRACE(DeviceManager_impl, "Softpkg type " << type)

    // Normalize type into either device or service
    // This is contrary to the spec, but existing devices/service may depend
    // on this behavior
    if ((type == "device") ||
        (type == "loadabledevice") ||
        (type == "executabledevice")) {
        type = "device";
    } 

    if ((type == "device") || (type == "service")) {
        supported = true;
    }
    else {
        LOG_ERROR(DeviceManager_impl, 
                  "Attempt to launch unsupported component type " << type)
    }

    return supported;
}




/*
 * Parsing constructor
 *
 * Parse the device manager configuration files.
 *
 * Register with the Domain Manager
 *
 * Loop through through the DeviceManager's associated devices and
 * create a thread for each device.
 */
void DeviceManager_impl::postConstructor (
        const char* overrideDomainName) 
    throw (CORBA::SystemException, std::runtime_error)

{
    myObj = _this();

    // Create the device file system in the DeviceManager POA.
    LOG_TRACE(DeviceManager_impl, "Creating device file system")
    FileSystem_impl* fs_servant = new FileSystem_impl(_fsroot.c_str());
    PortableServer::POA_var poa = ossie::corba::RootPOA()->find_POA("DeviceManager", 1);
    PortableServer::ObjectId_var oid = poa->activate_object(fs_servant);
    fs_servant->_remove_ref();
    _fileSys = fs_servant->_this();
    fileSysIOR = ossie::corba::objectToString(_fileSys);

    // create filesystem for local dom root.. used for softpkgs
    FileSystem_impl *local_dom_fs = new FileSystem_impl(_local_domroot.c_str());
    oid = poa->activate_object(local_dom_fs);
    local_dom_fs->_remove_ref();
    _local_dom_filesys = local_dom_fs->_this();

    // 
    // setup DeviceManager context from dcd, software profile, find matching implementation
    // and allocation properties
    //
    parseDeviceConfigurationProfile(overrideDomainName);

    parseSpd();

    setupImplementationForHost();

    getDomainManagerReferenceAndCheckExceptions();

    registerDeviceManagerWithDomainManager(myObj);
    
    resolveNamingContext();
    
    bindNamingContext();
    
    // Now that we've successfully communicated with the DomainManager, allow
    // for 1 retry in the event that it crashes and recovers, leaving us with a
    // valid reference but a stale connection.
    ossie::corba::setObjectCommFailureRetries(_dmnMgr, 1);

    //
    // Establish registration with the Domain's IDM_Channel that will be used to 
    // notify Device state changes....
    //
    ossie::events::EventChannelManager_var   ecm;
    ossie::events::EventRegistration         ereg;
    ereg.channel_name = CORBA::string_dup("IDM_Channel");
    try {
      ecm = _dmnMgr->eventChannelMgr();
      if ( ossie::corba::objectExists(ecm) ) {
        idm_registration = ecm->registerResource( ereg );
        IDM_IOR.clear();
      }
      else {
        // try fallback method
        idm_registration->channel = ossie::events::connectToEventChannel(rootContext, "IDM_Channel");
        if (CORBA::is_nil(idm_registration->channel)) {
          LOG_INFO(DeviceManager_impl, "IDM channel not found. Continuing without using the IDM channel");
        } else {
          IDM_IOR = ossie::corba::objectToString(idm_registration->channel);
        }
      }
    }
    catch(...){
        LOG_INFO(DeviceManager_impl, "IDM channel not found. Continuing without using the IDM channel");
    }

    _adminState = DEVMGR_REGISTERED;

    // create device manager cache location
    std::string devmgrcache(_cacheroot + "/." + _label);
    LOG_TRACE(DeviceManager_impl, "Creating DevMgr cache: " << devmgrcache)
    bool retval = this->makeDirectory(devmgrcache);
    if (not retval) {
        std::ostringstream eout;
        eout << "Unable to create the Device Manager cache: " << devmgrcache;
        LOG_ERROR(DeviceManager_impl, eout.str())
        throw std::runtime_error(eout.str().c_str());
    }

    //Parse local components from DCD files
    LOG_TRACE(DeviceManager_impl, "Grabbing component placements")
    const std::vector<ossie::ComponentPlacement>& componentPlacements = node_dcd.getComponentPlacements();
    LOG_TRACE(DeviceManager_impl, "ComponentPlacement size is " << componentPlacements.size())

    ////////////////////////////////////////////////////////////////////////////
    // Split component placements by compositePartOf tag
    //      The following logic exists below:
    //      - Split non-deployOnDevice from deployOnDevice compPlacements
    //      - Iterate and launch all non-deployOnDevice compPlacements
    //      - Iterate and launch all deployOnDevice compPlacements
    DeploymentList standaloneComponentPlacements;
    DeploymentList  compositePartDeviceComponentPlacements;
    ComponentPlacements::const_iterator constCompPlaceIter;
    for (constCompPlaceIter =  componentPlacements.begin();
         constCompPlaceIter != componentPlacements.end();
         constCompPlaceIter++) {
        
         local_spd::ProgramProfile *newResource = 0;
         const ComponentPlacement &componentPlacement = *constCompPlaceIter;
         std::string compId(constCompPlaceIter->instantiations[0].getID());
	 std::ostringstream emsg;
         emsg << "Skipping instantiation of device " << compId;
         try {
             // load up device/service software profile
             LOG_TRACE(DeviceManager_impl, "Getting file name for refid " << componentPlacement.getFileRefId());
             const char* spdFile = node_dcd.getFileNameFromRefId(componentPlacement.getFileRefId());
             newResource = local_spd::ProgramProfile::LoadProfile( _fileSys, spdFile, _local_dom_filesys );             

             // check if we have matching implementation
             if ( !resolveImplementation( newResource ) )  {
	       std::ostringstream eout;
	       eout  << "Device '" << compId  << "' - '" << newResource->getID() << "; "
		     << "No available device implementations match device manager " << devmgr_info->getID();
	       throw std::runtime_error(eout.str().c_str());
             }

             local_spd::ImplementationInfo *matchingImpl = newResource->selectedImplementation();        
             // resolve soft package dependenices for matching implementation
             if ( !resolveSoftpkgDependencies(matchingImpl) ) {
	       std::ostringstream eout;
	       eout  << "Device '" << compId  << "' - '" << newResource->getID() << "; "
		     << "No available softpkg dependenices match device manager implementation" << devmgr_info->getID();
	       throw std::runtime_error(eout.str().c_str());
             }

             bool isSharedLibrary = (matchingImpl->getCodeType() == CF::LoadableDevice::SHARED_LIBRARY);
	     bool isCompositePartOf = constCompPlaceIter->isCompositePartOf();
             Deployment d( (*constCompPlaceIter), newResource );

	     if (isCompositePartOf && isSharedLibrary) {
                 compositePartDeviceComponentPlacements.push_back( d );
	     } else {
                 standaloneComponentPlacements.push_back( d );
	     }

         }
         catch ( std::runtime_error &ex ) {
             LOG_ERROR(DeviceManager_impl, ex.what() );
             LOG_ERROR(DeviceManager_impl, emsg.str() );
            if (newResource) delete newResource;
            continue;
         }
         catch ( ... ) {
             LOG_ERROR(DeviceManager_impl, emsg.str() );
             if (newResource) delete newResource;
             continue;
         }
    }

    ////////////////////////////////////////////////////////////////////////////
    // Iterate and launch all non-deployOnDevice compPlacements
    DeploymentList::const_iterator cIter;
    for (cIter =  standaloneComponentPlacements.begin();
         cIter != standaloneComponentPlacements.end();
         cIter++) {
      
      const ComponentPlacement &compPlacement = cIter->first;
      local_spd::ProgramProfile *compProfile = cIter->second;
      const local_spd::ImplementationInfo *matchingImpl = compProfile->getSelectedImplementation();
      std::string compId(compPlacement.instantiations[0].getID());
      LOG_INFO(DeviceManager_impl, "Placing Component CompId: " << compId << " ProfileName : " << compProfile->getName() );

      // should not happen
      if (!matchingImpl) continue;

      ossie::Properties deviceProperties;
      if (!addDeviceImplProperties( compProfile, *matchingImpl )) {
	LOG_INFO(DeviceManager_impl, "Skipping instantiation of device '" << compProfile->getInstantiationIdentifier() << 
		 ", failed to merge properties ");
	continue;
      }

      std::string compositeDeviceIOR;
      getCompositeDeviceIOR(compositeDeviceIOR, 
			    componentPlacements, 
			    compPlacement);

      std::vector<ComponentInstantiation>::const_iterator cpInstIter;
      for (cpInstIter =  compPlacement.getInstantiations().begin(); 
	   cpInstIter != compPlacement.getInstantiations().end(); 
	   cpInstIter++) {

	const ComponentInstantiation instantiation = *cpInstIter;
	LOG_TRACE(DeviceManager_impl, "Placing component id: " << instantiation.getID());

        // setup profile with instantiation context
        recordComponentInstantiationId(instantiation, matchingImpl->getId());
        std::ostringstream identifier;
        identifier << instantiation.getID() << ":" << node_dcd.getName();
        compProfile->setIdentifier( instantiation.getID(), instantiation.getID());
        compProfile->setNamingServiceName(instantiation.getFindByNamingServiceName());
        compProfile->setUsageName(instantiation.getUsageName());
        compProfile->setAffinity( instantiation.getAffinity() );
        compProfile->setLoggingConfig( instantiation.getLoggingConfig() );

	//spawn device
	std::string codeFilePath;
	if (!getCodeFilePath(codeFilePath,
			     *matchingImpl,
			     compProfile->spd,
			     fs_servant)) {
	  continue;
	}

	std::string componentType;
	if (!getDeviceOrService(componentType, compProfile )) {
	  // We got a type other than "device" or "service"
	  continue;
	}

	// add to list of deployed resources
        {
            SCOPED_LOCK(componentImplMapmutex);
            deployed_comps.push_back( *cIter );
	}
        // Attempt to create the requested device or service
        createDeviceThreadAndHandleExceptions(compPlacement,
					      compProfile,
					      componentType,
					      codeFilePath,
					      instantiation,
					      compositeDeviceIOR );
      }
    }

    ////////////////////////////////////////////////////////////////////////////
    // Iterate and launch all deployOnDevice compPlacements
    DeploymentList::const_iterator compPlaceIter;
    for (compPlaceIter =  compositePartDeviceComponentPlacements.begin();
         compPlaceIter != compositePartDeviceComponentPlacements.end();
         compPlaceIter++) {

      const ComponentPlacement &compPlacement = compPlaceIter->first;
      local_spd::ProgramProfile *compProfile = compPlaceIter->second;
      std::string compId("UT OHHH");
      // get Device Manager implementation
      const char* compositePartDeviceID = compPlacement.getCompositePartOfDeviceID();
      const local_spd::ImplementationInfo *matchingImpl = compProfile->getSelectedImplementation();
      const local_spd::ImplementationInfo *parentImpl=0;
      
      if ( compPlacement.instantiations.size() > 0 ) {
          compId = compPlacement.instantiations[0].getID();
      }
      else {
      LOG_FATAL(DeviceManager_impl, "Missing Instantiaion for Placing Composite ParentCompId: " << compositePartDeviceID << " ProfileName : " << compProfile->getName() );
      }

      LOG_INFO(DeviceManager_impl, "Placing Composite ParentCompId: " << compositePartDeviceID << " ProfileName : " << compProfile->getName() << " CompID " << compId );

        bool foundCompositeDeployed = false;
        for (cIter =  standaloneComponentPlacements.begin();
             cIter != standaloneComponentPlacements.end();
             cIter++) {

            const ComponentPlacement &parentPlacement = cIter->first;
            local_spd::ProgramProfile *parentProfile = cIter->second;

            const std::vector<ComponentInstantiation> &parentInstantiations = parentPlacement.getInstantiations();
            std::vector<ComponentInstantiation>::const_iterator compInstIter;
            for (compInstIter = parentInstantiations.begin();
                 compInstIter != parentInstantiations.end();
                 compInstIter++) {

                std::string parent_inst_id(compInstIter->getID());

                if ( parent_inst_id == std::string(compositePartDeviceID)) {
                    parentImpl = parentProfile->getSelectedImplementation();
		  
                    // make sure parent was deployed...
                    {
                        SCOPED_LOCK(componentImplMapmutex);                        
                        DeploymentList::iterator i=deployed_comps.begin();
                        for ( ; i != deployed_comps.end(); i++ ) {
                            const std::vector<ComponentInstantiation> &pinst = i->first.getInstantiations();
                            std::vector<ComponentInstantiation>::const_iterator piter = pinst.begin();
                            for ( ; piter != pinst.end(); piter++ ){ 
                                std::string d_inst_id(piter->getID());
                                if ( parent_inst_id == d_inst_id ) {
                                    foundCompositeDeployed = true;
                                }
                            }

                        }
                    }
                    break;

                }
            }

            if (foundCompositeDeployed == false) {
                LOG_ERROR(DeviceManager_impl,
                          "Unable to locate ComppositeParent '" << compositePartDeviceID << " for '" << compositePartDeviceID << "'... Skipping instantiation of '" << compId );
                continue;
            }

            if (matchingImpl == NULL) {
                LOG_ERROR(DeviceManager_impl,
                          "Skipping instantiation of device '" << compId << "' - '" << compProfile->spd.getSoftPkgID() << "; "
                          << "no available device implementations match device manager properties")
                    continue;
            }

            if (parentImpl == NULL) {
                LOG_ERROR(DeviceManager_impl,
                          "Skipping instantiation of device '" << compId << "' - '" << compProfile->spd.getSoftPkgID() << "; "
                          << "Composite parent has no matching implementations")
                    continue;
            }

            // store the matchedDeviceImpl's implementation ID in a map for use with "getComponentImplementationId"
            if (!addDeviceImplProperties(compProfile, *matchingImpl)) {
                LOG_ERROR(DeviceManager_impl,"Skipping instantiation of device '" << compId << "' - '" << compProfile->spd.getSoftPkgID() << "'");
                continue;
            }

            std::string compositeDeviceIOR;
            getCompositeDeviceIOR(compositeDeviceIOR,
                                  componentPlacements,
                                  compPlacement);

            std::vector<ComponentInstantiation>::const_iterator cpInstIter =compPlacement.instantiations.begin();

            for (; cpInstIter != compPlacement.instantiations.end(); cpInstIter++) {

                const ComponentInstantiation instantiation = *cpInstIter;

                // setup profile with instantiation context
                recordComponentInstantiationId(instantiation, matchingImpl->getId());
                std::ostringstream identifier;
                identifier << instantiation.getID() << ":" << node_dcd.getName();
                //compProfile->setIdentifier( identifier.str().c_str(), instantiation.getID());
                compProfile->setIdentifier( instantiation.getID(), instantiation.getID() );
                compProfile->setNamingServiceName(instantiation.getFindByNamingServiceName());
                compProfile->setUsageName(instantiation.getUsageName());
                compProfile->setAffinity( instantiation.getAffinity() );
                compProfile->setLoggingConfig( instantiation.getLoggingConfig() );

                // Set Code file path
                std::string codeFilePath;
                if ( !getCodeFilePath(codeFilePath, *matchingImpl, compProfile->spd,fs_servant,false ) ) {
                    continue;
                }

                {
                    SCOPED_LOCK(componentImplMapmutex);
                    deployed_comps.push_back( *compPlaceIter );
                }

                // Set ComponentType
                std::string componentType = "SharedLibrary"; 
                // Attempt to create the requested device or service
                createDeviceThreadAndHandleExceptions(
                                                      compPlacement,
                                                      compProfile,
                                                      componentType,
                                                      codeFilePath,
                                                      instantiation,
                                                      compositeDeviceIOR );
            }
        }
    }
}

const std::vector<const Property*>&  DeviceManager_impl::getAllocationProperties() {
    return devmgr_info->prf.getAllocationProperties();
}

bool DeviceManager_impl::resolveSoftpkgDependencies(  local_spd::ImplementationInfo *implementation ) {
  return resolveSoftpkgDependencies( implementation, devmgr_info->prf );
}
 
bool DeviceManager_impl::resolveSoftpkgDependencies( local_spd::ImplementationInfo *implementation, 
                                                     const ossie::Properties &host_props )
                                                     
{
    const local_spd::SoftpkgInfoList & tmpSoftpkg = implementation->getSoftPkgDependencies();
    local_spd::SoftpkgInfoList::const_iterator iterSoftpkg;
    for (iterSoftpkg = tmpSoftpkg.begin(); iterSoftpkg != tmpSoftpkg.end(); ++iterSoftpkg) {
        // Find an implementation whose dependencies match
        const local_spd::SoftpkgInfo *pkg = *iterSoftpkg;
        local_spd::ImplementationInfo* spdImplInfo = resolveDependencyImplementation(*pkg, host_props);
        if (spdImplInfo) {
            const_cast< local_spd::SoftpkgInfo* >(pkg)->setSelectedImplementation(spdImplInfo);
            LOG_DEBUG(DeviceManager_impl, "resolveSoftpkgDependencies: selected: " << pkg->getName());
        } else {
            LOG_DEBUG(DeviceManager_impl, "resolveSoftpkgDependencies: implementation match not found between soft package dependency and device");
            implementation->clearSelectedDependencyImplementations();
            return false;
        }
    }

    return true;
}

local_spd::ImplementationInfo* DeviceManager_impl::resolveDependencyImplementation( const local_spd::SoftpkgInfo &softpkg,
                                                                                   const ossie::Properties &host_props )
{
    local_spd::ImplementationInfo::List spd_list;
    softpkg.getImplementations(spd_list);

    for (size_t implCount = 0; implCount < spd_list.size(); implCount++) {
        local_spd::ImplementationInfo *implementation = spd_list[implCount];
        // Check that this implementation can run on the device
        if (!implementation->checkProcessorAndOs(host_props)) {
            continue;
        }

        // Recursively check any softpkg dependencies
        if (resolveSoftpkgDependencies(implementation, host_props)) {
            return implementation;
        }
    }

    return 0;
}


bool DeviceManager_impl::resolveImplementation( local_spd::ProgramProfile *rsc ) {

    bool result = false;
    local_spd::ImplementationInfo::List  impls;
    rsc->getImplementations(impls);

    local_spd::ImplementationInfo::List::iterator itr = impls.begin();
    for ( ; itr != impls.end(); itr++ ) {
        local_spd::ImplementationInfo *impl = *itr;
        LOG_TRACE(DeviceManager_impl, 
                  "Attempting to match device " << rsc->getName() 
                  << " implementation id: " <<  impl->getId()
                  << " to device manager " << devmgr_info->getInstantiationIdentifier() );

        if ( impl->checkProcessorAndOs( devmgr_info->prf ) ) {
            rsc->setSelectedImplementation( impl );
            result = true;
            LOG_TRACE(DeviceManager_impl, 
                      "found matching processing device implementation,  device " 
                      << rsc->getName()    << " implementation id: " <<  impl->getId() );
            break;
        }
    }
    
    LOG_TRACE(DeviceManager_impl, "Done finding matching device implementation");
    return result;
}




bool DeviceManager_impl::addDeviceImplProperties (local_spd::ProgramProfile *compProfile, 
                                                  const local_spd::ImplementationInfo& deviceImpl )
{

    // store location of implementation specific PRF file
    ossie::Properties devProps;
    const std::string prfFile = deviceImpl.getPropertyFile();    
    if (prfFile.size()) {
        LOG_TRACE(DeviceManager_impl, "deviceImplProps: Joining implementation-specific PRF file " << prfFile);
        if (!joinPRFProperties(prfFile, devProps )) {
            return false;
        }
    } else {
        LOG_TRACE(DeviceManager_impl, "deviceImplProps: Device does not provide implementation-specific PRF file");
    }

    // merge props together...
    compProfile->prf.join(devProps);

    // 
    LOG_TRACE(DeviceManager_impl, "deviceImplProps: Adding factory params");
        const std::vector<const Property*>& fprop = devProps.getFactoryParamProperties();
        for (unsigned int i = 0; i < fprop.size(); i++) {
            compProfile->addFactoryParameter(convertPropertyToDataType(fprop[i]));
        }

    LOG_TRACE(DeviceManager_impl, "deviceImpProps: Adding exec params");
    const std::vector<const Property*>& eprop = devProps.getExecParamProperties();
    for (unsigned int i = 0; i < eprop.size(); i++) {
        if (std::string(eprop[i]->getMode()) != "readonly") {
            LOG_TRACE(DeviceManager_impl, "deviceImplProps: Adding exec param " << eprop[i]->getID() << " " << eprop[i]->getName());
            compProfile->addExecParameter(convertPropertyToDataType(eprop[i]));
        } else {
            LOG_TRACE(DeviceManager_impl, "deviceImplProps: Ignoring readonly exec param " << eprop[i]->getID() << " " << eprop[i]->getName());
        }
    }

    const std::vector<const Property*>& prop = devProps.getConfigureProperties();
    for (unsigned int i = 0; i < prop.size(); i++) {
        if (!prop[i]->isReadOnly()) {
            LOG_TRACE(DeviceManager_impl, "deviceImplProps: Adding configure prop " << prop[i]->getID() << " " << prop[i]->getName() << " " << prop[i]->isReadOnly())
                compProfile->addConfigureProperty(convertPropertyToDataType(prop[i]));
        }
    }

    const std::vector<const Property*>& cprop = devProps.getConstructProperties();
    for (unsigned int i = 0; i < cprop.size(); i++) {
        LOG_TRACE(DeviceManager_impl, "deviceImplProps: Adding construct prop " << cprop[i]->getID() << " " << cprop[i]->getName() << " " << cprop[i]->isReadOnly());
        if (cprop[i]->isCommandLine()) {
            compProfile->addExecParameter(convertPropertyToDataType(cprop[i]));
        } else {
            compProfile->addConstructProperty(convertPropertyToDataType(cprop[i]));
        }
    }

    return true;
}


bool DeviceManager_impl::joinPRFProperties (const std::string& prfFile, ossie::Properties& properties)
{
    try {
        // Check for the existence of the PRF file first so we can give a more meaningful error message.
        if (!_fileSys->exists(prfFile.c_str())) {
            LOG_ERROR(DeviceManager_impl, "PRF file " << prfFile << " does not exist");
        } else {
            LOG_TRACE(DeviceManager_impl, "Loading PRF file " << prfFile);
            File_stream prfStream(_fileSys, prfFile.c_str());
            properties.join(prfStream);
            LOG_TRACE(DeviceManager_impl, "Loaded PRF file " << prfFile);
            prfStream.close();
            return true;
        }
    } catch (const ossie::parser_error& ex) {
        std::string parser_error_line = ossie::retrieveParserErrorLineNumber(ex.what());
        LOG_ERROR(DeviceManager_impl, "Error parsing PRF: " << prfFile << ". " << parser_error_line << " The XML parser returned the following error: " << ex.what());
    } CATCH_LOG_ERROR(DeviceManager_impl, "Failure parsing PRF: " << prfFile);

    return false;
}

void
DeviceManager_impl::getDomainManagerReference (const std::string& domainManagerName)
{
    CORBA::Object_var obj = CORBA::Object::_nil();

    bool warned = false;
    do {
        try {
            obj = ossie::corba::objectFromName(domainManagerName);
        } catch (const CosNaming::NamingContext::NotFound&) {
            if (!warned) {
                warned = true;
                LOG_WARN(DeviceManager_impl, "DomainManager not registered with NameService; retrying");
            }
        } catch( CORBA::SystemException& se ) {
            LOG_ERROR(DeviceManager_impl, "[DeviceManager::getDomainManagerReference] \"get_object_from_name\" failed with CORBA::SystemException")
            throw;
        } catch ( std::exception& ex ) {
            LOG_ERROR(DeviceManager_impl, "The following standard exception occurred: "<<ex.what()<<" while attempting \"get_object_from_name\"")
            throw;
        } catch ( const CORBA::Exception& ex ) {
            LOG_ERROR(DeviceManager_impl, "The following CORBA exception occurred: "<<ex._name()<<" while attempting \"get_object_from_name\"")
            throw;
        } catch( ... ) {
            LOG_ERROR(DeviceManager_impl, "[DeviceManager::getDomainManagerReference] \"get_object_from_name\" failed with Unknown Exception")
            throw;
        }

        // Sleep for a tenth of a second to give the DomainManager a chance to
        // bind itself into the naming context.
        usleep(10000);

        // If a shutdown occurs while waiting, turn it into an exception.
        if (*_internalShutdown) {
            throw std::runtime_error("Interrupted waiting to lookup DomainManager in NameService");
        }
    } while(CORBA::is_nil(obj));

    try {
        _dmnMgr = CF::DomainManager::_narrow (obj);
        LOG_TRACE(DeviceManager_impl, "Accessing DomainManager : " << domainManagerName);
    } catch ( std::exception& ex ) {
        LOG_ERROR(DeviceManager_impl, "The following standard exception occurred: "<<ex.what()<<" while attempting to narrow on the Domain Manager")
        throw;
    } catch ( const CORBA::Exception& ex ) {
        LOG_ERROR(DeviceManager_impl, "The following CORBA exception occurred: "<<ex._name()<<" while attempting to narrow on the Domain Manager")
        throw;
    } catch( ... ) {
        LOG_ERROR(DeviceManager_impl, "[DeviceManager::getDomainManagerReference] \"CF:DomainManager::_narrow\" failed with Unknown Exception")
        throw;
    }
}


char* DeviceManager_impl::deviceConfigurationProfile ()
throw (CORBA::SystemException)
{
    return CORBA::string_dup(_deviceConfigurationProfile.c_str());
}


CF::FileSystem_ptr DeviceManager_impl::fileSys ()throw (CORBA::
                                                        SystemException)
{
    CF::FileSystem_var result = _fileSys;
    return result._retn();
}


char* DeviceManager_impl::identifier ()
throw (CORBA::SystemException)
{
    return CORBA::string_dup (_identifier.c_str());
}


char* DeviceManager_impl::label ()
throw (CORBA::SystemException)
{
    return CORBA::string_dup (_label.c_str());
}


CF::DomainManager_ptr DeviceManager_impl::domMgr ()throw (CORBA::
                                                        SystemException)
{
    return CF::DomainManager::_duplicate(this->_dmnMgr);
}


CF::DeviceManager::ServiceSequence *
DeviceManager_impl::registeredServices ()throw (CORBA::SystemException)
{
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

    CF::DeviceManager::ServiceSequence_var result;

    try {
        result = new CF::DeviceManager::ServiceSequence();
        result->length(_registeredServices.size());
        for (CORBA::ULong ii = 0; ii < _registeredServices.size(); ++ii){
            result[ii].serviceObject = CORBA::Object::_duplicate(_registeredServices[ii]->service);
            result[ii].serviceName = _registeredServices[ii]->label.c_str();
        }
    } catch ( ... ){
        result = new CF::DeviceManager::ServiceSequence();
    }

    return result._retn();
}


void
DeviceManager_impl::registerDevice (CF::Device_ptr registeringDevice)
throw (CORBA::SystemException, CF::InvalidObjectReference)
{
  if (CORBA::is_nil (registeringDevice)) {
    LOG_WARN(DeviceManager_impl, "Attempted to register NIL device")
      throw (CF::InvalidObjectReference("[DeviceManager::registerDevice] Cannot register Device. registeringDevice is a nil reference."));
  }

  if (*_internalShutdown) // do not service a registration request if the Device Manager is shutting down
    return;

  ossie::corba::overrideBlockingCall(registeringDevice,getClientWaitTime());
  std::string deviceLabel = ossie::corba::returnString(registeringDevice->label());
  std::string device_id = ossie::corba::returnString(registeringDevice->identifier());
  LOG_INFO(DeviceManager_impl, "Registering device " << deviceLabel << " device id " << device_id << " on Device Manager " << _label);

  if ( deviceIsRegistered( registeringDevice ) == true ) {
    std::ostringstream eout;
    eout << "Device is already registred: "<< deviceLabel;
    LOG_WARN(DeviceManager_impl, eout.str());
    return;
  }

  local_spd::ProgramProfile *spdinfo=0;
  try { 
      spdinfo = findProfile( registeringDevice->identifier() );
  }
  catch(...) {
      std::ostringstream eout;
      eout << "Loading Device's SPD failed, device:" <<  deviceLabel;
          LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
  } 

  // This lock needs to be here because we add the device to
  // the registeredDevices list at the top...therefore
  // getting the registeredDevices attribute could
  // show the device as registered before it actually gets
  // registered.
  //
  // This lock should be after as may CORBA calls as possible
  // (e.g., registeringDevice->label()) in case omniORB blocks
  // due to a lack of threads (which would result in blocking
  // the mutex lock, which would prevent shutdown from killing
  // this).
  boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

  //Get properties from SPD
  std::string spdFile = ossie::corba::returnString(registeringDevice->softwareProfile());
  std::string spd_name = spdinfo->getName();
  std::string spd_id = spdinfo->getID();
  std::string deviceid = ossie::corba::returnString(registeringDevice->identifier());
  LOG_INFO(DeviceManager_impl, "Device LABEL: " << deviceLabel << "  SPD loaded: " << spd_name << "' - '" << spd_id );


  //
  // call resource's initializeProperties method to handle any properties required for construction
  //
  if (spdinfo->isConfigurable ()) {
    try {
      //
      LOG_DEBUG(DeviceManager_impl, "Initialize properties for spd/device label: " << spd_name << "/" << deviceLabel);
      const CF::Properties cprops = spdinfo->getNonNilConstructProperties();
      for (unsigned int j = 0; j < cprops.length (); j++) {
        LOG_DEBUG(DeviceManager_impl, "initializeProperties prop id " << cprops[j].id );
      }
      // Try to set the initial values for the component's properties
      registeringDevice->initializeProperties(cprops);
    } catch(CF::PropertySet::InvalidConfiguration& e) {
      std::ostringstream eout;
      eout << "Device '" << deviceLabel << "' - '" << spd_id << "' may not have been initialized correctly; "
           << "Call to initializeProperties() resulted in InvalidConfiguration exception. Device registration with Device Manager failed";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    } catch(CF::PropertySet::PartialConfiguration& e) {
      std::ostringstream eout;
      eout << "Device '" << deviceLabel << "' - '" << spd_id << "' may not have been configured correctly; "
           << "Call to initializeProperties() resulted in PartialConfiguration exception.";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    } catch ( std::exception& ex ) {
      std::ostringstream eout;
      eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to initalizeProperties for  "<<deviceLabel<<". Device registration with Device Manager failed";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    } catch ( const CORBA::Exception& ex ) {
      std::ostringstream eout;
      eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to initializeProperties for "<<deviceLabel<<". Device registration with Device Manager failed";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    } catch( ... ) {
      std::ostringstream eout;
      eout << "Failed to initialize device properties: '";
      eout << deviceLabel << "' with device id: '" << spd_id;
      eout << "'initializeProperties' failed with Unknown Exception" << "Device registration with Device Manager failed ";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    }
  }

  LOG_DEBUG(DeviceManager_impl, "Initializing device " << deviceLabel << " on Device Manager " << _label);
  try {
    registeringDevice->initialize();
  } catch (CF::LifeCycle::InitializeError& ex) {
    std::ostringstream eout;
    eout << "Device "<< deviceLabel << " threw a CF::LifeCycle::InitializeError exception"<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( std::exception& ex ) {
    std::ostringstream eout;
    eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to initialize Device " << deviceLabel<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( const CORBA::Exception& ex ) {
    std::ostringstream eout;
    eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to initialize Device " << deviceLabel<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  }

  //configure properties
  try {  
    LOG_DEBUG(DeviceManager_impl, "Configuring device " << deviceLabel << " on Device Manager " << _label);
      const CF::Properties cprops  = spdinfo->getNonNilConfigureProperties();
    LOG_TRACE(DeviceManager_impl, "Listing configuration properties");
    for (unsigned int j=0; j<cprops.length(); j++) {
      LOG_TRACE(DeviceManager_impl, "Prop id " << cprops[j].id );
    }
    if (cprops.length() != 0)
        registeringDevice->configure (cprops);
  } catch (CF::PropertySet::PartialConfiguration& ex) {
    std::ostringstream eout;
    eout << "Device '" << deviceLabel << "' - '" << spd_id << "' may not have been configured correctly; "
         << "Call to configure() resulted in PartialConfiguration exception.";
    LOG_ERROR(DeviceManager_impl, eout.str())
      throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch (CF::PropertySet::InvalidConfiguration& ex) {
    std::ostringstream eout;
    eout << "Device '" << deviceLabel << "' - '" << spd_id << "' may not have been configured correctly; "
         << "Call to configure() resulted in InvalidConfiguration exception. Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( std::exception& ex ) {
    std::ostringstream eout;
    eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to configure "<<deviceLabel<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( const CORBA::Exception& ex ) {
    std::ostringstream eout;
    eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to configure "<<deviceLabel<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  }

  // Register the device with the Device manager, unless it is already
  // registered
  if (!deviceIsRegistered (registeringDevice)) {
    // if the device is not registered, then add it to the naming context
    LOG_TRACE(DeviceManager_impl, "Binding device to name " << deviceLabel)
        CosNaming::Name_var device_name = ossie::corba::stringToName(deviceLabel.c_str());
    try {
      devMgrContext->bind(device_name, registeringDevice);
    } catch ( ... ) {
      // there is already something bound to that name
      // from the perspective of this framework implementation, the multiple names are not acceptable
      // consider this a registered device
      LOG_WARN(DeviceManager_impl, "Device is already registered");
      return;
    }
    increment_registeredDevices(registeringDevice);
  } else {
    LOG_WARN(DeviceManager_impl, "Device is already registered");
    return;
  }

  // If this Device Manager is registered with a Domain Manager, register
  // the new device with the Domain Manager
  if (_adminState == DEVMGR_REGISTERED) { 
      try {
          LOG_INFO(DeviceManager_impl, "Registering device " << deviceLabel << " on Domain Manager " << _domainName );
          _dmnMgr->registerDevice (registeringDevice, myObj);
      } catch( CF::DomainManager::RegisterError& e ) {
          LOG_ERROR(DeviceManager_impl, "Failed to register device to domain manager due to: " << e.msg);
      } catch ( std::exception& ex ) {
          LOG_ERROR(DeviceManager_impl, "The following standard exception occurred: "<<ex.what()<<" while attempting to register with the Domain Manager")
              } catch( const CORBA::Exception& e ) {
          LOG_ERROR(DeviceManager_impl, "Failed to register device to domain manager due to: " << e._name());
      }
  } else {
      LOG_WARN(DeviceManager_impl, "Skipping DomainManager registerDevice because the device manager isn't registered")
        }

  LOG_TRACE(DeviceManager_impl, "Done registering device " << deviceLabel);

  //The registerDevice operation shall write a FAILURE_ALARM log record to a
  //DomainManagers Log, upon unsuccessful registration of a Device to the DeviceManagers
  //registeredDevices.
}




void DeviceManager_impl::registerRogueDevice (CF::Device_ptr registeringDevice)
{

  //Get properties from SPD
  std::string spdFile = ossie::corba::returnString(registeringDevice->softwareProfile());
  std::string deviceLabel = ossie::corba::returnString(registeringDevice->label());
  std::string device_id = ossie::corba::returnString(registeringDevice->identifier());

  // Open the SPD file using the SCA FileSystem
  LOG_TRACE(DeviceManager_impl, "Building DRogue Device Info From SPD File");
  std::auto_ptr< local_spd::ProgramProfile > spdinfo;
  try  {
    spdinfo = local_spd::ProgramProfile::LoadProgramProfile(_fileSys, spdFile.c_str(),_local_dom_filesys );
      
  }
  catch(...) {
    std::ostringstream eout;
    eout << "Loading Device's SPD failed, device:" <<  ossie::corba::returnString(registeringDevice->label());
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  }

  std::string spd_name = spdinfo->getName();
  std::string spd_id = spdinfo->getID();
  LOG_INFO(DeviceManager_impl, "Device LABEL: " << deviceLabel << "  SPD loaded: " << spd_name << "' - '" << spd_id );

  CF::Properties componentProperties;
  DeviceManagerConfiguration DCDParser;
  try {
    File_stream _dcd(_fileSys, _deviceConfigurationProfile.c_str());
    DCDParser.load(_dcd);
    _dcd.close();
  } catch ( std::exception& ex ) {
    std::ostringstream eout;
    eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to parse "<<_deviceConfigurationProfile;
    LOG_ERROR(DeviceManager_impl, eout.str())
      throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( const CORBA::Exception& ex ) {
    std::ostringstream eout;
    eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to parse "<<_deviceConfigurationProfile;
    LOG_ERROR(DeviceManager_impl, eout.str())
      throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( ... ) {
    std::ostringstream eout;
    eout << "[DeviceManager::registerDevice] Failed to parse DCD";
    LOG_ERROR(DeviceManager_impl, eout.str())
      throw(CF::InvalidObjectReference(eout.str().c_str()));
  }

  // get properties from device PRF that matches the registering device
  std::string deviceid = ossie::corba::returnString(registeringDevice->identifier());
  try {
    const ComponentInstantiation& instantiation = DCDParser.getComponentInstantiationById(deviceid);
    if (instantiation.getUsageName() != NULL)
      std::string tmp_name = instantiation.getUsageName(); // this is here to get rid of a warning
  } catch (std::out_of_range& e) {
    std::ostringstream eout;
    eout << "[DeviceManager::registerDevice] Failed to parse DCD";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( std::exception& ex ) {
    std::ostringstream eout;
    eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to parse "<<_deviceConfigurationProfile;
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( const CORBA::Exception& ex ) {
    std::ostringstream eout;
    eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to parse "<<_deviceConfigurationProfile;
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  }

  // override device properties in DCD file
  const ComponentInstantiation& instantiation = DCDParser.getComponentInstantiationById(deviceid);
  const ossie::ComponentPropertyList& overrideProps = instantiation.getProperties();
  // Check for any overrides from DCD componentproperites
  for (unsigned int j = 0; j < overrideProps.size (); j++) {
    LOG_TRACE(DeviceManager_impl, "Override  Properties prop id " << overrideProps[j].getID());
    spdinfo->overrideProperty( overrideProps[j] );
  }

  //
  // call resource's initializeProperties method to handle any properties required for construction
  //
  if (spdinfo->isConfigurable ()) {
    try {
      //
      LOG_DEBUG(DeviceManager_impl, "Initialize properties for spd/device label: " << spd_name << "/" << deviceLabel);
      const CF::Properties cprops = spdinfo->getNonNilConstructProperties();
      for (unsigned int j = 0; j < cprops.length (); j++) {
        LOG_DEBUG(DeviceManager_impl, "initializeProperties prop id " << cprops[j].id );
      }
      // Try to set the initial values for the component's properties
      registeringDevice->initializeProperties(cprops);
    } catch(CF::PropertySet::InvalidConfiguration& e) {
      std::ostringstream eout;
      eout << "Device '" << deviceLabel << "' - '" << spd_id << "' may not have been initialized correctly; "
           << "Call to initializeProperties() resulted in InvalidConfiguration exception. Device registration with Device Manager failed";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    } catch(CF::PropertySet::PartialConfiguration& e) {
      std::ostringstream eout;
      eout << "Device '" << deviceLabel << "' - '" << spd_id << "' may not have been configured correctly; "
           << "Call to initializeProperties() resulted in PartialConfiguration exception.";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    } catch ( std::exception& ex ) {
      std::ostringstream eout;
      eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to initalizeProperties for  "<<deviceLabel<<". Device registration with Device Manager failed";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    } catch ( const CORBA::Exception& ex ) {
      std::ostringstream eout;
      eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to initializeProperties for "<<deviceLabel<<". Device registration with Device Manager failed";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    } catch( ... ) {
      std::ostringstream eout;
      eout << "Failed to initialize device properties: '";
      eout << deviceLabel << "' with device id: '" << spd_id;
      eout << "'initializeProperties' failed with Unknown Exception" << "Device registration with Device Manager failed ";
      LOG_ERROR(DeviceManager_impl, eout.str());
      throw(CF::InvalidObjectReference(eout.str().c_str()));
    }
  }

  LOG_DEBUG(DeviceManager_impl, "Initializing device " << deviceLabel << " on Device Manager " << _label);
  try {
    registeringDevice->initialize();
  } catch (CF::LifeCycle::InitializeError& ex) {
    std::ostringstream eout;
    eout << "Device "<< deviceLabel << " threw a CF::LifeCycle::InitializeError exception"<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( std::exception& ex ) {
    std::ostringstream eout;
    eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to initialize Device " << deviceLabel<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( const CORBA::Exception& ex ) {
    std::ostringstream eout;
    eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to initialize Device " << deviceLabel<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  }

  //configure properties
  try {  
    LOG_DEBUG(DeviceManager_impl, "Configuring device " << deviceLabel << " on Device Manager " << _label);
      const CF::Properties cprops  = spdinfo->getNonNilConfigureProperties();
    LOG_TRACE(DeviceManager_impl, "Listing configuration properties");
    for (unsigned int j=0; j<cprops.length(); j++) {
      LOG_TRACE(DeviceManager_impl, "Prop id " << cprops[j].id );
    }
    if (cprops.length() != 0)
        registeringDevice->configure (cprops);
  } catch (CF::PropertySet::PartialConfiguration& ex) {
    std::ostringstream eout;
    eout << "Device '" << deviceLabel << "' - '" << spd_id << "' may not have been configured correctly; "
         << "Call to configure() resulted in PartialConfiguration exception.";
    LOG_ERROR(DeviceManager_impl, eout.str())
      throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch (CF::PropertySet::InvalidConfiguration& ex) {
    std::ostringstream eout;
    eout << "Device '" << deviceLabel << "' - '" << spd_id << "' may not have been configured correctly; "
         << "Call to configure() resulted in InvalidConfiguration exception. Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( std::exception& ex ) {
    std::ostringstream eout;
    eout << "The following standard exception occurred: "<<ex.what()<<" while attempting to configure "<<deviceLabel<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  } catch ( const CORBA::Exception& ex ) {
    std::ostringstream eout;
    eout << "The following CORBA exception occurred: "<<ex._name()<<" while attempting to configure "<<deviceLabel<<". Device registration with Device Manager failed";
    LOG_ERROR(DeviceManager_impl, eout.str());
    throw(CF::InvalidObjectReference(eout.str().c_str()));
  }

  // Register the device with the Device manager, unless it is already
  // registered
  if (!deviceIsRegistered (registeringDevice)) {
    // if the device is not registered, then add it to the naming context
    LOG_TRACE(DeviceManager_impl, "Binding device to name " << deviceLabel)
        CosNaming::Name_var device_name = ossie::corba::stringToName(deviceLabel.c_str());
    try {
      devMgrContext->bind(device_name, registeringDevice);
    } catch ( ... ) {
      // there is already something bound to that name
      // from the perspective of this framework implementation, the multiple names are not acceptable
      // consider this a registered device
      LOG_WARN(DeviceManager_impl, "Device is already registered");
      return;
    }
    increment_registeredDevices(registeringDevice);
  } else {
    LOG_WARN(DeviceManager_impl, "Device is already registered");
    return;
  }

  // If this Device Manager is registered with a Domain Manager, register
  // the new device with the Domain Manager
  if (_adminState == DEVMGR_REGISTERED) { 
    try {
      LOG_INFO(DeviceManager_impl, "Registering device " << deviceLabel << " on Domain Manager");
      _dmnMgr->registerDevice (registeringDevice, myObj);
    } catch( CF::DomainManager::RegisterError& e ) {
      LOG_ERROR(DeviceManager_impl, "Failed to register device to domain manager due to: " << e.msg);
    } catch ( std::exception& ex ) {
      LOG_ERROR(DeviceManager_impl, "The following standard exception occurred: "<<ex.what()<<" while attempting to register with the Domain Manager")
        } catch( const CORBA::Exception& e ) {
      LOG_ERROR(DeviceManager_impl, "Failed to register device to domain manager due to: " << e._name());
    }
  } else {
    LOG_WARN(DeviceManager_impl, "Skipping DomainManager registerDevice because the device manager isn't registered")
      }

  LOG_TRACE(DeviceManager_impl, "Done registering device " << deviceLabel);

  //The registerDevice operation shall write a FAILURE_ALARM log record to a
  //DomainManagers Log, upon unsuccessful registration of a Device to the DeviceManagers
  //registeredDevices.

}



//This function returns TRUE if the input serviceName is contained in the _registeredServices list attribute
bool DeviceManager_impl::serviceIsRegistered (const char* serviceName)
{
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);
    for (ServiceList::const_iterator serviceIter = _registeredServices.begin(); serviceIter != _registeredServices.end(); ++serviceIter){
        if (strcmp((*serviceIter)->label.c_str(), serviceName) == 0){
            return true;
        }
    }
    return false;
}


void
DeviceManager_impl::unregisterDevice (CF::Device_ptr registeredDevice)
throw (CORBA::SystemException, CF::InvalidObjectReference)
{
    TRACE_ENTER(DeviceManager_impl)

    std::string dev_id;
    std::string dev_name;

    bool deviceFound = false;
    if (CORBA::is_nil (registeredDevice)) {       //|| !deviceIsRegistered(registeredDevice) )
//The unregisterDevice operation shall write a FAILURE_ALARM log record, when it cannot
//successfully remove a registeredDevice from the DeviceManagers registeredDevices.

//The unregisterDevice operation shall raise the CF InvalidObjectReference when the input
//registeredDevice is a nil CORBA object reference or does not exist in the DeviceManagers
//registeredDevices attribute.
        /*writeLogRecord(FAILURE_ALARM,invalid reference input parameter.); */
        LOG_ERROR(DeviceManager_impl, "Attempt to unregister nil device")
        throw (CF::InvalidObjectReference("Cannot unregister Device. registeringDevice is a nil reference."));
    }

//The unregisterDevice operation shall remove the input registeredDevice from the
//DeviceManagers registeredDevices attribute.
    try {
        dev_id = ossie::corba::returnString(registeredDevice->identifier());
        dev_name = ossie::corba::returnString(registeredDevice->label());
    } catch ( std::exception& ex ) {
        LOG_ERROR(DeviceManager_impl, "The following standard exception occurred: "<<ex.what()<<" while trying to retrieve the identifier and label of the registered device")
        throw(CF::InvalidObjectReference());
    } catch ( const CORBA::Exception& ex ) {
        LOG_ERROR(DeviceManager_impl, "The following CORBA exception occurred: "<<ex._name()<<" while trying to retrieve the identifier and label of the registered device")
        throw(CF::InvalidObjectReference());
    } catch ( ... ) {
        throw (CF:: InvalidObjectReference("Cannot Unregister Device. Invalid reference"));
    }

//Look for registeredDevice in _registeredDevices
    deviceFound = decrement_registeredDevices(registeredDevice);
    if (!deviceFound) {
        /*writeLogRecord(FAILURE_ALARM,invalid reference input parameter.); */
        LOG_ERROR(DeviceManager_impl, "Cannot unregister Device. registeringDevice was not registered.")
        throw (CF::InvalidObjectReference("Cannot unregister Device. registeringDevice was not registered."));
    }

    TRACE_EXIT(DeviceManager_impl);
}

void DeviceManager_impl::deleteFileSystems()
{
    PortableServer::POA_var poa = ossie::corba::RootPOA()->find_POA("DeviceManager", 0);
    PortableServer::ObjectId_var oid = poa->reference_to_id(_fileSys);
    poa->deactivate_object(oid);
    _fileSys = CF::FileSystem::_nil();
}

void
DeviceManager_impl::shutdown ()
throw (CORBA::SystemException)
{
    *_internalShutdown = true;
    LOG_DEBUG(DeviceManager_impl, "SHUTDOWN START........." << *_internalShutdown)

    if ((_adminState == DEVMGR_SHUTTING_DOWN) || (_adminState == DEVMGR_SHUTDOWN)) {
        LOG_DEBUG(DeviceManager_impl, "SHUTTIING DOWN NOW......" );
        return;
    }

  _adminState = DEVMGR_SHUTTING_DOWN;

    // SR:501
    // The shutdown operation shall unregister the DeviceManager from the DomainManager.
    // Although unclear, a failure here should NOT prevent us from trying to clean up
    // everything per SR::503
    try {
        CF::DeviceManager_var self = _this();
        if ( !CORBA::is_nil(_dmnMgr ) ) {
            _dmnMgr->unregisterDeviceManager(self);
            LOG_DEBUG(DeviceManager_impl, "SHUTDOWN ......... unregisterDeviceManager ");
        }
    } catch( ... ) {
    }

    //
    // release any event channels that we registered against
    //
    try {
        if ( !CORBA::is_nil(_dmnMgr ) ) {
            CF::EventChannelManager_var ecm = _dmnMgr->eventChannelMgr();
            if ( CORBA::is_nil(ecm) == false && idm_registration.operator->() != NULL ){
                LOG_INFO(DeviceManager_impl, "Unregister IDM CHANNEL:" << idm_registration->reg.reg_id);
                ecm->unregister( idm_registration->reg );
            }
            LOG_DEBUG(DeviceManager_impl, "SHUTDOWN ......... Unregister IDM_CHANNEL");
        }
    }catch(...){
    }

    // SR:502
    //The shutdown operation shall perform releaseObject on all of the DeviceManagers registered
    //Devices (DeviceManagers registeredDevices attribute).
    // releaseObject for AggregateDevices calls releaseObject on all of their child devices
    // ergo a while loop must be used vice a for loop
    clean_registeredServices();
    clean_externalServices();
    clean_registeredDevices();

    LOG_DEBUG(DeviceManager_impl, "SHUTDOWN ......... Unbinding device manager context");
    try {
        CosNaming::Name devMgrContextName;
        devMgrContextName.length(1);
        devMgrContextName[0].id = CORBA::string_dup(_label.c_str());
        if (!CORBA::is_nil(rootContext)) {
            rootContext->unbind(devMgrContextName);
        }
    } catch ( ... ) {
    }

    try {
        deleteFileSystems();
    } catch ( ... ) {
    }


    try {
        _adminState = DEVMGR_SHUTDOWN;
    } catch ( ... ) {
    }

    LOG_DEBUG(DeviceManager_impl, "SHUTDOWN ......... completed");
}

void
DeviceManager_impl::registerService (CORBA::Object_ptr registeringService,
                                     const char* name)
throw (CORBA::SystemException, CF::InvalidObjectReference)
{
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);
    LOG_INFO(DeviceManager_impl, "Registering service " << name)

    if (CORBA::is_nil (registeringService)) {
        throw (CF::InvalidObjectReference("Cannot register service, registeringService is a nil reference."));
    }

    ossie::corba::overrideBlockingCall(registeringService,getClientWaitTime());

    // Register the service with the Device manager, unless it is already
    // registered
    if (!serviceIsRegistered(name)) {
        // Per the specification, service usagenames are not optional and *MUST* be
        // unique per each service type.  Therefore, a domain cannot have two
        // services of the same usagename.
        LOG_TRACE(DeviceManager_impl, "Binding service to name " << name);
        CosNaming::Name_var service_name = ossie::corba::stringToName(name);
        try {
             rootContext->rebind(service_name, registeringService);
        } catch ( ... ) {
            // there is already something bound to that name
            // from the perspective of this framework implementation, the multiple names are not acceptable
            // consider this a registered device
            LOG_WARN(DeviceManager_impl, "Service is already registered")
            return;
        }

        increment_registeredServices(registeringService, name);

    } else {
        LOG_WARN(DeviceManager_impl, "Service is already registered")
        return;
    }

    //The registerService operation shall register the registeringService with the DomainManager
    //when the DeviceManager has already registered to the DomainManager and the
    //registeringService has been successfully added to the DeviceManagers registeredServices
    //attribute.
    if (_adminState == DEVMGR_REGISTERED) {
        try {
            _dmnMgr->registerService(registeringService, myObj, name);
        } catch ( ... ) {
            CosNaming::Name_var service_name = ossie::corba::stringToName(name);
            rootContext->unbind(service_name);
            _registeredServices.pop_back();
            LOG_ERROR(DeviceManager_impl, "Failed to register service to the domain manager; unregistering the service from the device manager")
            throw;
        }
    }

//The registerService operation shall write a FAILURE_ALARM log record, upon unsuccessful
//registration of a Service to the DeviceManagers registeredServices.
//The registerService operation shall raise the CF InvalidObjectReference exception when the
//input registeringService is a nil CORBA object reference.
}

void
DeviceManager_impl::unregisterService (CORBA::Object_ptr registeredService,
                                       const char* name)
throw (CORBA::SystemException, CF::InvalidObjectReference)
{
    LOG_INFO(DeviceManager_impl, "Unregistering service " << name)

    if (CORBA::is_nil (registeredService)) {
        /*writeLogRecord(FAILURE_ALARM,invalid reference input parameter.); */
        throw (CF::InvalidObjectReference("Cannot unregister Service. registeringService is a nil reference."));
    }

    //Look for registeredDevice in _registeredDevices
    bool serviceFound = decrement_registeredServices(registeredService, name);
    if (serviceFound)
        return;


//If it didn't find registeredDevice, then throw an exception
    /*writeLogRecord(FAILURE_ALARM,invalid reference input parameter.);*/
    throw (CF::InvalidObjectReference("Cannot unregister Service. registeringService was not registered."));
//The unregisterService operation shall write a FAILURE_ALARM log record, when it cannot
//successfully remove a registeredService from the DeviceManagers registeredServices.
//The unregisterService operation shall raise the CF InvalidObjectReference when the input
//registeredService is a nil CORBA object reference or does not exist in the DeviceManagers
//registeredServices attribute.
}

local_spd::ProgramProfile *DeviceManager_impl::findProfile (const std::string &componentInstantiationId)
{
    SCOPED_LOCK(componentImplMapmutex);
    local_spd::ProgramProfile *ret=0;
    DeploymentList::iterator iter;
    for( iter=deployed_comps.begin(); iter != deployed_comps.end(); iter++ ) {
        std::string cid = iter->second->getInstantiationIdentifier();
        LOG_TRACE(DeviceManager_impl, "Looking for Profile match: RegisteringInstanceID/ProfileInstanceId: " << componentInstantiationId << 
                  " / " << iter->second->getInstantiationIdentifier() );
        if ( componentInstantiationId == cid ) {
            LOG_TRACE(DeviceManager_impl, "Looking for Profile FOUND MATCH " << 
                       iter->second->getInstantiationIdentifier() );
            ret=iter->second;
            break;
        }
    }
    return ret;
}




char * DeviceManager_impl::getComponentImplementationId (const char* componentInstantiationId)
throw (CORBA::SystemException)
{
//The getComponentImplementationId operation shall return the SPD implementation elements
//ID attribute that matches the SPD implementation element used to create the component
//identified by the input componentInstantiationId parameter.

    SCOPED_LOCK(componentImplMapmutex);

    std::map <std::string, std::string>::iterator map_iter;

    // make sure componentInstantiationId is in the map
    map_iter = _componentImplMap.find(componentInstantiationId);
    if (map_iter != _componentImplMap.end()) {
        // return associated SPD implementation element id
        return CORBA::string_dup(_componentImplMap[componentInstantiationId].c_str());
    } else {
        return CORBA::string_dup("");
    }

//The getComponentImplementationId operation shall return an empty string when the input
//componentInstantiationId parameter does not match the ID attribute of any SPD implementation
//element used to create the component.
}

bool DeviceManager_impl::makeDirectory(std::string path)
{
    bool done = false;

    std::string initialDir;
    if (path[0] == '/') {
        initialDir = "/";
    } else {
        initialDir = "";
    }

    std::string workingFileName;

    if (path[path.length()-1] == '/') {
        workingFileName = path;
    } else {
        workingFileName = path + "/";
    }
    std::string::size_type begin_pos = 0;
    std::string::size_type last_slash = workingFileName.find_last_of("/");
    
    bool success = true;

    if (last_slash != std::string::npos) {
        while (!done) {
            std::string::size_type pos = workingFileName.find_first_of("/", begin_pos);
            if (pos == begin_pos) { // first slash - don't do anything
                begin_pos++;
                continue;
            }
            if (pos == std::string::npos) //
                { break; }

            initialDir += workingFileName.substr(begin_pos, (pos - begin_pos)) + std::string("/");
            int retval = mkdir(initialDir.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
            if (retval == -1) {
                if (errno == ENOENT) {
                    LOG_WARN(DeviceManager_impl, "Failed to create directory (from " << workingFileName << ") " << initialDir <<". Non-existent root directory.")
                    success = false;
                } else if (errno == EEXIST) {
                    LOG_TRACE(DeviceManager_impl, "Directory (from " << workingFileName << ") " << initialDir <<" already exists. No need to make a new one.")
                } else if (errno == EACCES) {
                    LOG_WARN(DeviceManager_impl, "Failed to create directory (from " << workingFileName << ") " << initialDir <<". Please check your write permissions.")
                    success = false;
                } else if (errno == ENOTDIR) {
                    LOG_WARN(DeviceManager_impl, "Failed to create directory (from " << workingFileName << ") " << initialDir <<". One of the components of the path is not a directory.")
                    success = false;
                } else if (errno == ELOOP) {
                    LOG_WARN(DeviceManager_impl, "Failed to create directory (from " << workingFileName << ") " << initialDir <<". A loop exists in the symbolic links in the path.")
                    success = false;
                } else if (errno == EMLINK) {
                    LOG_WARN(DeviceManager_impl, "Failed to create directory (from " << workingFileName << ") " << initialDir <<". The link count of the parent directory exceeds LINK_MAX.")
                    success = false;
                } else if (errno == ENAMETOOLONG) {
                    LOG_WARN(DeviceManager_impl, "Failed to create directory (from " << workingFileName << ") " << initialDir <<". The path name is too long.")
                    success = false;
                } else if (errno == EROFS) {
                    LOG_WARN(DeviceManager_impl, "Failed to create directory (from " << workingFileName << ") " << initialDir <<". This is a read-only file system.")
                    success = false;
                } else {
                    LOG_WARN(DeviceManager_impl, "Attempt to create directory (from " << workingFileName << ") " << initialDir <<" failed with the following error number: " << errno)
                    success = false;
                }
            } else {
                LOG_TRACE(DeviceManager_impl, "Creating directory (from " << workingFileName << ") " << initialDir)
            }
            begin_pos = pos + 1;
        }
    }
    bool retval = checkWriteAccess(path);
    if (not retval) {
        LOG_ERROR(DeviceManager_impl, "The Device Manager (or one of its children) does not have write permission to one or more files in the cache.")
        return false;
    }
    return success;
}

bool DeviceManager_impl::checkWriteAccess(std::string &path)
{
    DIR *dp;
    struct dirent *ep;
    dp = opendir(path.c_str());
    if (dp == NULL) {
        if (errno == ENOENT) {
            LOG_WARN(DeviceManager_impl, "Failed to create directory " << path <<".")
        } else if (errno == EACCES) {
            LOG_WARN(DeviceManager_impl, "Failed to create directory " << path <<". Please check your write permissions.")
        } else if (errno == ENOTDIR) {
            LOG_WARN(DeviceManager_impl, "Failed to create directory " << path <<". One of the components of the path is not a directory.")
        } else if (errno == EMFILE) {
            LOG_WARN(DeviceManager_impl, "Failed to create directory " << path <<". Too many file descriptors open by the process.")
        } else if (errno == ENFILE) {
            LOG_WARN(DeviceManager_impl, "Failed to create directory " << path <<". Too many file descriptors open by the system.")
        } else if (errno == ENOMEM) {
            LOG_WARN(DeviceManager_impl, "Failed to create directory " << path <<". Insufficient memory to complete the operation.")
        } else {
            LOG_WARN(DeviceManager_impl, "Attempt to create directory " << path <<" failed with the following error number: " << errno)
        }
        return false;
    }
    while ((ep = readdir(dp)) != NULL) {
        std::string name = ep->d_name;
        if ((name == ".") or (name == "..")) continue;
        std::string full_name = path + "/" + name;
        if (access(full_name.c_str(), W_OK) == -1) {
            LOG_WARN(DeviceManager_impl, "The file '" << full_name << "' cannot be overwritten by the Device Manager process (or one of its children).")
            (void) closedir(dp);
            return false;
        }
        if (ep->d_type == DT_DIR) {
            bool retval = checkWriteAccess(full_name);
            if (not retval) {
                (void) closedir(dp);
                return retval;
            }
        }
    }
    (void) closedir(dp);
    return true;
}

/****************************************************************************

 The following functions manage the _registeredDevices sequence as well
  as a couple of associated data structures (the have to be synchronized)

****************************************************************************/

bool DeviceManager_impl::decrement_registeredServices(CORBA::Object_ptr registeredService, const char* name)
{
    bool serviceFound = false;

    //The unregisterService operation shall remove the input registeredService from the
    //DeviceManagers registeredServices attribute. The unregisterService operation shall unregister
    //the input registeredService from the DomainManager when the input registeredService is
    //registered with the DeviceManager and the DeviceManager is not in the shutting down state.

    // Acquire the registered device mutex so that no one else can read of modify the list.
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

    for (ServiceList::iterator serviceIter = _registeredServices.begin(); serviceIter != _registeredServices.end(); ++serviceIter){
        ServiceNode* serviceNode = *serviceIter;
        if (strcmp((*serviceIter)->label.c_str(), name) == 0){
            serviceFound = true;

            local_unregisterService(registeredService, name);

            // Remove the service from the list of registered services
            _registeredServices.erase(serviceIter);

            std::string label = serviceNode->label;
            if (serviceNode->pid != 0){
                // The service process has not terminated, so add it back to the pending list.
                _pendingServices.push_back(serviceNode);
            }

            break;
        }
    }


    return serviceFound;
}

void DeviceManager_impl::local_unregisterService(CORBA::Object_ptr service, const std::string& name)
{
    // Unbind service from the naming service

    // Per the specification, service usagenames are not optional and *MUST* be
    // unique per each service type.  Therefore, a domain cannot have two
    // services of the same usagename.
    CosNaming::Name_var tmpServiceName = ossie::corba::stringToName(name);
    try {
        rootContext->unbind(tmpServiceName);
    } catch ( ... ){
    }

    // Ddon't unregisterService from the domain manager if we are SHUTTING_DOWN
    if (_adminState == DEVMGR_REGISTERED){
        try {
            _dmnMgr->unregisterService(service, name.c_str());
        } catch ( ... ) {
        }
    }
}

bool DeviceManager_impl::decrement_registeredDevices(CF::Device_ptr registeredDevice)
{
    bool deviceFound = false;
    const std::string deviceIOR = ossie::corba::objectToString(registeredDevice);

    // Acquire the registered device mutex so that no one else can read or modify the list.
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);
    for (DeviceList::iterator deviceIter = _registeredDevices.begin(); 
         deviceIter != _registeredDevices.end(); 
         ++deviceIter) {
        DeviceNode* deviceNode = *deviceIter;
        if (deviceNode->IOR == deviceIOR) {
            deviceFound = true;
            
            // Remove device from the list of registered devices.
            _registeredDevices.erase(deviceIter);

            std::string label = deviceNode->label;
            if (deviceNode->pid != 0) {
                // The device process has not terminated, so add it back to the pending list.
                _pendingDevices.push_back(deviceNode);
            }

            // Release the registered device mutex, now that we are done modifying the list. If we unregister
            // the device from the DomainManager, it will call 'registeredDevices()', which requires the mutex.
            lock.unlock();

            local_unregisterDevice(registeredDevice, label);
            
            break;
        }
    }
    
    return deviceFound;
}

void DeviceManager_impl::local_unregisterDevice(CF::Device_ptr device, const std::string& label)
{
  try {
    // Unbind device from the naming service
    CosNaming::Name_var tmpDeviceName = ossie::corba::stringToName(label);
    devMgrContext->unbind(tmpDeviceName);
  } CATCH_LOG_ERROR(DeviceManager_impl, "Unable to unbind device: " << label )

  // Per SR:490, don't unregisterDevice from the domain manager if we are SHUTTING_DOWN
  if (_adminState == DEVMGR_REGISTERED) {
    try {
      _dmnMgr->unregisterDevice(device);
    } catch( ... ) {
    }
  }

}

/*
 * increment the registered services sequences along with the id and table tables
 */
void DeviceManager_impl::increment_registeredServices(CORBA::Object_ptr registeringService, const char* name)
{
    // Find the device in the pending list. If we launched the device process, it should be found here.
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

    ServiceNode* serviceNode = 0;
    for (ServiceList::iterator serviceIter = _pendingServices.begin(); serviceIter != _pendingServices.end(); ++serviceIter) {
        if (strcmp((*serviceIter)->label.c_str(), name) == 0){
            serviceNode = *serviceIter;
            _pendingServices.erase(serviceIter);
            break;
        }
    }

    if (!serviceNode){
        // A service is registering that was not launched by this DeviceManager. Create a node
        // to manage it, but mark the PID as 0, as there is no process to monitor.
        LOG_WARN(DeviceManager_impl, "Registering service " << name << " was not launched by this DeviceManager");
        serviceNode = new ServiceNode;
        serviceNode->identifier = name;
        serviceNode->pid = 0;
    }

    //The registerService operation shall add the input registeringService to the DeviceManagers
    //registeredServices attribute when the input registeringService does not already exist in the
    //registeredServices attribute. The registeringService is ignored when duplicated.
    serviceNode->label = name;
    serviceNode->IOR = ossie::corba::objectToString(registeringService);
    serviceNode->service = CORBA::Object::_duplicate(registeringService);

    _registeredServices.push_back(serviceNode);
}

/*
* increment the registered devices sequences along with the id and label tables
*/
void DeviceManager_impl::increment_registeredDevices(CF::Device_ptr registeringDevice)
{
    const std::string identifier = ossie::corba::returnString(registeringDevice->identifier());

    // Find the device in the pending list. If we launched the device process, it should be found here.
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);
    DeviceNode* deviceNode = 0;
    for (DeviceList::iterator deviceIter = _pendingDevices.begin(); 
         deviceIter != _pendingDevices.end(); 
         ++deviceIter) {

        if ((*deviceIter)->identifier == identifier) {
            deviceNode = *deviceIter;
            _pendingDevices.erase(deviceIter);
            break;
        }
    }

    if (!deviceNode) {
        // A device is registering that was not launched by this DeviceManager. Create a node
        // to manage it, but mark the PID as 0, as there is no process to monitor.
        LOG_WARN(DeviceManager_impl, "Registering device " << identifier << " was not launched by this DeviceManager");
        deviceNode = new DeviceNode;
        deviceNode->identifier = identifier;
        deviceNode->pid = 0;
    }

    // Fill in the device node fields that were not known at launch time (label has probably
    // not changed, but we consider the device authoritative).
    deviceNode->label = ossie::corba::returnString(registeringDevice->label());
    deviceNode->IOR = ossie::corba::objectToString(registeringDevice);
    deviceNode->device = CF::Device::_duplicate(registeringDevice);

    _registeredDevices.push_back(deviceNode);
}

/*
* This function returns TRUE if the input registeredDevice is contained in the _registeredDevices list attribute
*/
bool DeviceManager_impl::deviceIsRegistered (CF::Device_ptr registeredDevice)
{
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);
    for (DeviceList::const_iterator deviceIter = _registeredDevices.begin(); 
         deviceIter != _registeredDevices.end(); 
         ++deviceIter) {

        if ((*deviceIter)->device->_is_equivalent(registeredDevice)) {
            return true;
        }
    }
    return false;
}

CF::DeviceSequence* DeviceManager_impl::registeredDevices () throw (CORBA::SystemException)
{
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);
    CF::DeviceSequence_var result;
    try {
        result = new CF::DeviceSequence();
        result->length(_registeredDevices.size());
        for (CORBA::ULong ii = 0; ii < _registeredDevices.size(); ++ii) {
            result[ii] = CF::Device::_duplicate(_registeredDevices[ii]->device);
        }
    } catch ( ... ) {
        result = new CF::DeviceSequence();
    }
    return result._retn();
}

std::string DeviceManager_impl::getIORfromID(const char* instanceid)
{
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

    for (DeviceList::const_iterator deviceIter = _registeredDevices.begin(); 
         deviceIter != _registeredDevices.end(); 
         ++deviceIter) {

        if ((*deviceIter)->identifier == instanceid) {
            return (*deviceIter)->IOR;
        }
    }
    return std::string();
}


/* Removes any services that were registered from an external source  */
void DeviceManager_impl::clean_externalServices(){
    ServiceNode* serviceNode = 0;
    {
        boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

        for (ServiceList::iterator serviceIter = _registeredServices.begin(); serviceIter != _registeredServices.end(); ++serviceIter) {
            if ((*serviceIter)->pid == 0) {
                serviceNode = (*serviceIter);
                break;
            }
        }
    }

    if (serviceNode){
        local_unregisterService(serviceNode->service, serviceNode->label);
    }
}

void DeviceManager_impl::clean_registeredServices(){

    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);
    std::vector<unsigned int> pids;

    for (ServiceList::iterator serviceIter = _registeredServices.begin(); serviceIter != _registeredServices.end(); ++serviceIter) {
        pids.push_back((*serviceIter)->pid);
    }
    for (ServiceList::iterator serviceIter = _pendingServices.begin(); serviceIter != _pendingServices.end(); ++serviceIter) {
        pids.push_back((*serviceIter)->pid);
    }

    // Clean up service processes.
    for (ServiceList::iterator serviceIter = _pendingServices.begin(); serviceIter != _pendingServices.end(); ++serviceIter) {
        pid_t servicePid = (*serviceIter)->pid;

        // Try an orderly shutdown.
        // NOTE: If the DeviceManager was terminated with a ^C, sending this signal may cause the
        //       original SIGINT to be forwarded to all other children (which is harmless, but be aware).
        kill(servicePid, SIGTERM);
    }

    // Send a SIGTERM to any services that haven't yet unregistered
    for (ServiceList::iterator serviceIter = _registeredServices.begin(); serviceIter != _registeredServices.end(); ++serviceIter){
        pid_t servicePid = (*serviceIter)->pid;
        // Only kill services that were launched by this device manager
        if (servicePid != 0){
            kill(servicePid, SIGTERM);
        }
    }

    lock.unlock();

    // Release the lock and allow time for the devices to exit.
    if (pids.size() != 0) {
        struct timeval tmp_time;
        struct timezone tmp_tz;
        gettimeofday(&tmp_time, &tmp_tz);
        double wsec_begin = tmp_time.tv_sec;
        double fsec_begin = tmp_time.tv_usec / 1e6;
        double wsec_end = wsec_begin;
        double fsec_end = fsec_begin;
        double time_diff = (wsec_end + fsec_end)-(wsec_begin + fsec_begin);
        bool registered_pending_pid_gone = false;
        while ((time_diff < 0.5) and (not registered_pending_pid_gone)) {
            registered_pending_pid_gone = true;
            for (std::vector<unsigned int>::iterator p_pid = pids.begin(); p_pid != pids.end(); ++p_pid) {
                if (kill(*p_pid, 0) != -1) {
                    registered_pending_pid_gone = false;
                    break;
                }
            }
            if (not registered_pending_pid_gone) {
                gettimeofday(&tmp_time, &tmp_tz);
                wsec_end = tmp_time.tv_sec;
                fsec_end = tmp_time.tv_usec / 1e6;
                time_diff = (wsec_end + fsec_end)-(wsec_begin + fsec_begin);
                usleep(1000);
            }
        }
    }
    lock.lock();

    // Send a SIGKILL to any remaining services.
    for (ServiceList::iterator serviceIter = _pendingServices.begin(); serviceIter != _pendingServices.end(); ++serviceIter) {
        pid_t servicePid = (*serviceIter)->pid;
        kill(servicePid, SIGKILL);
    }

    // Send a SIGKILL to any services that haven't yet unregistered
    for (ServiceList::iterator serviceIter = _registeredServices.begin(); serviceIter != _registeredServices.end(); ++serviceIter){
        pid_t servicePid = (*serviceIter)->pid;
        // Only kill services that were launched by this device manager
        if (servicePid != 0){
            kill(servicePid, SIGKILL);
        }
    }
}

void DeviceManager_impl::clean_registeredDevices()
{
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);
    while (!_registeredDevices.empty()) {
        DeviceNode* deviceNode = _registeredDevices[0];
        const std::string label = deviceNode->label;
        CF::Device_var deviceRef = CF::Device::_duplicate(deviceNode->device);

        // Temporarily release the mutex while calling releaseObject, which
        // should update the registered devices list; it is possible that the
        // device node will be deleted before the lock is re-acquired, so local
        // copies of any objects must be used
        LOG_INFO(DeviceManager_impl, "Releasing device " << label);
        lock.unlock();
        try {
            // 3 seconds or use cfg option
            ossie::corba::overrideBlockingCall(deviceRef, 3000 );
            deviceRef->releaseObject();
        } catch ( ... ) {
        }
        lock.lock();

        // If the device is still at the front of the list, releaseObject must
        // have failed
        if (!_registeredDevices.empty() && (_registeredDevices[0] == deviceNode)) {
            // Remove the device from the registered list, moving it to the
            // pending list if it has a PID associated with it
            _registeredDevices.erase(_registeredDevices.begin());
            if (deviceNode->pid != 0) {
                _pendingDevices.push_back(deviceNode);
            } else {
                delete deviceNode;
            }
        }
    }

    LOG_DEBUG(DeviceManager_impl, "Sending SIGNAL TREE to to device process " );
    // Clean up device processes, starting with an orderly shutdown and
    // escalating as needed
    // NOTE: If the DeviceManager was terminated with a ^C, sending SIGINT may
    //       cause the original SIGINT to be forwarded to all other children
    //       (which is harmless, but be aware).
    float device_force_quite_time = this->DEVICE_FORCE_QUIT_TIME * 1e6;
    killPendingDevices(SIGINT, device_force_quite_time);
    killPendingDevices(SIGTERM, device_force_quite_time);
    killPendingDevices(SIGKILL, 0);
}

/*
 * Return a device node if the pid was found in either the _pendingDevices or
 * in the _registeredDevices.
 */
DeviceManager_impl::DeviceNode* DeviceManager_impl::getDeviceNode(const pid_t pid) {
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

    // Try to find a device that has already unregistered or has not yet registered.
    for (DeviceList::iterator deviceIter = _pendingDevices.begin(); 
         deviceIter != _pendingDevices.end(); 
         ++deviceIter) {
        if ((*deviceIter)->pid == pid) {
            DeviceNode* deviceNode = *deviceIter;
            _pendingDevices.erase(deviceIter);
            if (_pendingDevices.empty()) {
                pendingDevicesEmpty.notify_all();
            }
            return deviceNode;
        }
    }

    // If there was not an unregistered device, check if a registered device terminated early.
    for (DeviceList::iterator deviceIter = _registeredDevices.begin(); 
         deviceIter != _registeredDevices.end(); 
         ++deviceIter) {
        if ((*deviceIter)->pid == pid) {
            DeviceNode* deviceNode = *deviceIter;
            _registeredDevices.erase(deviceIter);
            local_unregisterDevice(deviceNode->device, deviceNode->label);
            return deviceNode;
        }
    }

    return 0;
}

void DeviceManager_impl::childExited (pid_t pid, int status)
{
    DeviceNode* deviceNode = getDeviceNode(pid);

    ServiceNode* serviceNode = 0;
    {
        boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

        // Try to find a service that has already unregistered
        for (ServiceList::iterator serviceIter = _pendingServices.begin(); serviceIter != _pendingServices.end(); ++serviceIter){
            if ((*serviceIter)->pid == pid){
                serviceNode = (*serviceIter);
                _pendingServices.erase(serviceIter);
                break;
            }
        }

        // If there was not an unregistered device, check if a registered device terminated early.
        if (!serviceNode) {
            for (ServiceList::iterator serviceIter = _registeredServices.begin(); serviceIter != _registeredServices.end(); ++serviceIter) {
                if ((*serviceIter)->pid == pid) {
                    serviceNode = (*serviceIter);
                    _registeredServices.erase(serviceIter);
                    // If a service terminated unexpectedly, unregister it.
                    local_unregisterService(serviceNode->service, serviceNode->label);
                    break;
                }
            }
        }
    }

    // The pid should always be found; if it is not, it must be a logic error.
    if (!deviceNode && !serviceNode) {
        LOG_ERROR(DeviceManager_impl, "Process " << pid << " is not associated with a registered device");
        return;
    }

    std::string label;
    if (deviceNode){
        label = deviceNode->label;
    } else {
        label = serviceNode->label;
    }

    if (WIFSIGNALED(status)) {
        if (deviceNode) {
            LOG_WARN(DeviceManager_impl, "Child process " << label << " (pid " << pid << ") has terminated with signal " << WTERMSIG(status));
        } else { // it's a service, so no termination through signal is the correct behavior
            LOG_INFO(DeviceManager_impl, "Child process " << label << " (pid " << pid << ") has terminated with signal " << WTERMSIG(status));
        }
    } else {
        LOG_INFO(DeviceManager_impl, "Child process " << label << " (pid " << pid << ") has exited with status " << WEXITSTATUS(status));
    }

    if (deviceNode) {
        delete deviceNode;
    } else {
        delete serviceNode;
    }
}

bool DeviceManager_impl::allChildrenExited ()
{
    boost::recursive_mutex::scoped_lock lock(registeredDevicesmutex);

    if ((_pendingDevices.size() == 0) && (_registeredDevices.size() == 0) &&
        (_pendingServices.size() == 0) && (_registeredServices.size() == 0)
        ) {
        return true;
    }

    return false;
}
