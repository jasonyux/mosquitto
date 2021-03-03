/*
Copyright (c) 2009-2020 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License 2.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   https://www.eclipse.org/legal/epl-2.0/
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

SPDX-License-Identifier: EPL-2.0 OR EDL-1.0

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#include <stdio.h>
#include <string.h>

#include "mosquitto_broker_internal.h"
#include "memory_mosq.h"
#include "mqtt_protocol.h"
#include "packet_mosq.h"
#include "property_mosq.h"
#include "send_mosq.h"

/* code added by Xiao */
char *strip_prefix(char *text, char* prefix)
{
	char *tmp = text;
	char *tmp_prefix = prefix;
	/* strip the local prefix for the db generated name*/
	size_t strip_count = strlen(prefix);
	while(*text && *tmp_prefix && *text++ == *tmp_prefix++)
		strip_count--;
	/* resets if not matching */
	if(strip_count)
		text = tmp;
	return text;
}

int recursive_sub(char *from_id, char *to_id)
{
	char *prefix = "ubuntu.", *tok;
	char *mapping_symbol = "->";

	char *from_id_cpy = malloc(strlen(from_id)+1);
	char *to_id_cpy = malloc(strlen(to_id)+1);
	strcpy(from_id_cpy, from_id);
	strcpy(to_id_cpy, to_id);

	char *from_id_cpy_tmp = strip_prefix(from_id_cpy, prefix);
	char *to_id_cpy_tmp = strip_prefix(to_id_cpy, prefix);

	char *from_f_t[2];
	char *to_f_t[2];

	int i = 0;
	tok = strtok(from_id_cpy_tmp, mapping_symbol);
	while(tok && i<2){
		from_f_t[i++] = tok;
		tok = strtok(NULL, mapping_symbol);
	}
	
	int j = 0;
	tok = strtok(to_id_cpy_tmp, mapping_symbol);
	while(tok && j<2){
		to_f_t[j++] = tok;
		tok = strtok(NULL, mapping_symbol);
	}

	/* recursive if b1->b7 and b7->b1 */
	int result = 0;
	if(i==2 && j==2){
		printf("comparing %s->%s and %s->%s\n", from_f_t[0], from_f_t[1], to_f_t[0], to_f_t[1]);
		if((strcmp(from_f_t[0], to_f_t[1]) == 0) && (strcmp(from_f_t[1], to_f_t[0]) == 0)){
			result = 1;
		}
		printf("same (1=true)? %d\n", result);
	}
	
	free(from_id_cpy);
	free(to_id_cpy);

	return result;
}

/* end of code added by Xiao */

int handle__subscribe(struct mosquitto *context)
{
	int rc = 0;
	int rc2;
	uint16_t mid;
	char *sub;
	uint8_t subscription_options;
	uint32_t subscription_identifier = 0;
	uint8_t qos;
	uint8_t retain_handling = 0;
	uint8_t *payload = NULL, *tmp_payload;
	uint32_t payloadlen = 0;
	size_t len;
	uint16_t slen;
	char *sub_mount;
	mosquitto_property *properties = NULL;
	bool allowed;

	if(!context) return MOSQ_ERR_INVAL;

	if(context->state != mosq_cs_active){
		return MOSQ_ERR_PROTOCOL;
	}

	log__printf(NULL, MOSQ_LOG_DEBUG, "Received SUBSCRIBE from %s", context->id);

	if(context->protocol != mosq_p_mqtt31){
		if((context->in_packet.command&0x0F) != 0x02){
			return MOSQ_ERR_MALFORMED_PACKET;
		}
	}
	if(packet__read_uint16(&context->in_packet, &mid)) return MOSQ_ERR_MALFORMED_PACKET;
	if(mid == 0) return MOSQ_ERR_MALFORMED_PACKET;

	if(context->protocol == mosq_p_mqtt5){
		rc = property__read_all(CMD_SUBSCRIBE, &context->in_packet, &properties);
		if(rc){
			/* FIXME - it would be better if property__read_all() returned
			 * MOSQ_ERR_MALFORMED_PACKET, but this is would change the library
			 * return codes so needs doc changes as well. */
			if(rc == MOSQ_ERR_PROTOCOL){
				return MOSQ_ERR_MALFORMED_PACKET;
			}else{
				return rc;
			}
		}

		if(mosquitto_property_read_varint(properties, MQTT_PROP_SUBSCRIPTION_IDENTIFIER,
					&subscription_identifier, false)){

			/* If the identifier was force set to 0, this is an error */
			if(subscription_identifier == 0){
				mosquitto_property_free_all(&properties);
				return MOSQ_ERR_MALFORMED_PACKET;
			}
		}

		mosquitto_property_free_all(&properties);
		/* Note - User Property not handled */
	}

	while(context->in_packet.pos < context->in_packet.remaining_length){
		sub = NULL;
		if(packet__read_string(&context->in_packet, &sub, &slen)){
			mosquitto__free(payload);
			return MOSQ_ERR_MALFORMED_PACKET;
		}

		if(sub){
			if(!slen){
				log__printf(NULL, MOSQ_LOG_INFO,
						"Empty subscription string from %s, disconnecting.",
						context->address);
				mosquitto__free(sub);
				mosquitto__free(payload);
				return MOSQ_ERR_MALFORMED_PACKET;
			}
			if(mosquitto_sub_topic_check(sub)){
				log__printf(NULL, MOSQ_LOG_INFO,
						"Invalid subscription string from %s, disconnecting.",
						context->address);
				mosquitto__free(sub);
				mosquitto__free(payload);
				return MOSQ_ERR_MALFORMED_PACKET;
			}

			if(packet__read_byte(&context->in_packet, &subscription_options)){
				mosquitto__free(sub);
				mosquitto__free(payload);
				return MOSQ_ERR_MALFORMED_PACKET;
			}
			if(context->protocol == mosq_p_mqtt31 || context->protocol == mosq_p_mqtt311){
				qos = subscription_options;
				if(context->is_bridge){
					subscription_options = MQTT_SUB_OPT_RETAIN_AS_PUBLISHED | MQTT_SUB_OPT_NO_LOCAL;
				}
			}else{
				qos = subscription_options & 0x03;
				subscription_options &= 0xFC;

				retain_handling = (subscription_options & 0x30);
				if(retain_handling == 0x30 || (subscription_options & 0xC0) != 0){
					mosquitto__free(sub);
					mosquitto__free(payload);
					return MOSQ_ERR_MALFORMED_PACKET;
				}
			}
			if(qos > 2){
				log__printf(NULL, MOSQ_LOG_INFO,
						"Invalid QoS in subscription command from %s, disconnecting.",
						context->address);
				mosquitto__free(sub);
				mosquitto__free(payload);
				return MOSQ_ERR_MALFORMED_PACKET;
			}
			if(qos > context->max_qos){
				qos = context->max_qos;
			}


			if(context->listener && context->listener->mount_point){
				len = strlen(context->listener->mount_point) + slen + 1;
				sub_mount = mosquitto__malloc(len+1);
				if(!sub_mount){
					mosquitto__free(sub);
					mosquitto__free(payload);
					return MOSQ_ERR_NOMEM;
				}
				snprintf(sub_mount, len, "%s%s", context->listener->mount_point, sub);
				sub_mount[len] = '\0';

				mosquitto__free(sub);
				sub = sub_mount;

			}
			log__printf(NULL, MOSQ_LOG_DEBUG, "\t%s (QoS %d)", sub, qos);

			/* start of subscription flooding code */

			// if there are bridges configured
			if (db.bridge_count){
				printf("sending subscribe to %d bridges\n", db.bridge_count);
				for(int i=0; i<db.bridge_count; i++){
					int old_topics_num = db.bridges[i]->bridge->topic_count;
					struct mosquitto__bridge_topic *old_topics = db.bridges[i]->bridge->topics;
					/* It seems that subscribing multiple subscriptions is basically done by several calls of this to a single topic
					* Therefore, a fixed size would work. However, it would be an optimization task to accept multiple messages once.
					*/
					int new_topics_num = 1;
					int same_topic = 0;
					for (int j = 0; j < old_topics_num; j++){
						char *old_topic = old_topics[j].remote_topic;
						if (strlen(old_topic) == strlen(sub) && strncmp(old_topic, sub, strlen(sub)))
							same_topic = 1;
					}
					char *fromSender = context->id;
					char *toSender = db.bridges[i]->bridge->name;
					char *local_prefix = "local.";
					/* strip the local prefix for the db generated name*/
					toSender = strip_prefix(toSender, local_prefix);
					
					///* on error, just continue and not send it*/
					//if(snprintf(toSender, sizeof(toSender), "ubuntu.bridge-%d", db.config->listeners->port) <= 0)
					//	continue;

					/* send if:
						1. the topic is not redundant (compared to list of topics already subscribed)
						2. not sending the subscription back to the broker, who actually sent it to me
					 */
					/* char *expectedTemplate = "ubuntu.bridge-dddd";
					int cmpSize = strlen(expectedTemplate) < strlen(fromSender) ? strlen(expectedTemplate): strlen(fromSender);
					if (!same_topic && strncmp(fromSender, toSender, (size_t) cmpSize)){
						char *new_topics[new_topics_num];
						new_topics[0] = sub;
						send__subscribe(db.bridges[i], NULL, 1, new_topics, qos, NULL);
					} */

					if (!same_topic && !recursive_sub(toSender, fromSender)){
						printf("my subflooding send\n");
						char *new_topics[new_topics_num];
						new_topics[0] = sub;
						send__subscribe(db.bridges[i], NULL, 1, new_topics, qos, NULL);
					}

				}
			}

			/* end of subscription flooding code */

			allowed = true;
			rc2 = mosquitto_acl_check(context, sub, 0, NULL, qos, false, MOSQ_ACL_SUBSCRIBE);
			switch(rc2){
				case MOSQ_ERR_SUCCESS:
					break;
				case MOSQ_ERR_ACL_DENIED:
					allowed = false;
					if(context->protocol == mosq_p_mqtt5){
						qos = MQTT_RC_NOT_AUTHORIZED;
					}else if(context->protocol == mosq_p_mqtt311){
						qos = 0x80;
					}
					break;
				default:
					mosquitto__free(sub);
					return rc2;
			}

			if(allowed){
				rc2 = sub__add(context, sub, qos, subscription_identifier, subscription_options, &db.subs);
				if(rc2 > 0){
					mosquitto__free(sub);
					return rc2;
				}
				if(context->protocol == mosq_p_mqtt311 || context->protocol == mosq_p_mqtt31){
					if(rc2 == MOSQ_ERR_SUCCESS || rc2 == MOSQ_ERR_SUB_EXISTS){
						if(retain__queue(context, sub, qos, 0)) rc = 1;
					}
				}else{
					if((retain_handling == MQTT_SUB_OPT_SEND_RETAIN_ALWAYS)
							|| (rc2 == MOSQ_ERR_SUCCESS && retain_handling == MQTT_SUB_OPT_SEND_RETAIN_NEW)){

						if(retain__queue(context, sub, qos, subscription_identifier)) rc = 1;
					}
				}

				log__printf(NULL, MOSQ_LOG_SUBSCRIBE, "%s %d %s", context->id, qos, sub);
			}
			mosquitto__free(sub);

			tmp_payload = mosquitto__realloc(payload, payloadlen + 1);
			if(tmp_payload){
				payload = tmp_payload;
				payload[payloadlen] = qos;
				payloadlen++;
			}else{
				mosquitto__free(payload);

				return MOSQ_ERR_NOMEM;
			}
		}
	}

	if(context->protocol != mosq_p_mqtt31){
		if(payloadlen == 0){
			/* No subscriptions specified, protocol error. */
			return MOSQ_ERR_MALFORMED_PACKET;
		}
	}
	if(send__suback(context, mid, payloadlen, payload)) rc = 1;
	mosquitto__free(payload);

#ifdef WITH_PERSISTENCE
	db.persistence_changes++;
#endif

	if(context->current_out_packet == NULL){
		rc = db__message_write_queued_out(context);
		if(rc) return rc;
		rc = db__message_write_inflight_out_latest(context);
		if(rc) return rc;
	}

	return rc;
}


