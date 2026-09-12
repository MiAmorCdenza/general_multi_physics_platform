# plugins/formats: file formats -- documents, and the trace exporters beside them.
#
# A format is content, not mechanism: which one a document is saved as is a choice a
# user or a site makes, and a platform that baked one choice into `core/` has made
# that decision for everyone who comes after. What core owns is the question a format
# answers -- `authoring/persist` for documents, `runtime/io` for exported data -- and
# this directory holds the answers.
#
# No dependency between the members, deliberately. A format that needs another format
# to work is not a format.

if(QP_BUILD_PLUGINS)
    add_subdirectory(qpjson)
endif()
